/*
 * fp_provider.c — 指纹业务线程实现(FINGERPRINT_AS608.md §5 业务流程语义)
 *
 * 指令序列帧级依据:docs/tech/FINGERPRINT_PROTOCOL.md v1(§5 序列表)。
 * 一次按压一次判定:RELEASED 前不再触发新流程(§6 边界);录入是两次按压,
 * 查重插在按压①后,同指校验在按压②后,取消/退出回滚已 Store 模板。
 */
#include "fp_provider.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "fp_as608.h"
#include "registry.h"
#include "storage.h"
#include "timeutil.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[FINGER]";

static const char *SVC_NAME = "finger";

/* ---- 状态 ---- */
static volatile bool s_running;
static pthread_t s_tid;
static bool s_tid_created;
static bool s_ready;                      /* 链路握手通过 */
static bool s_fault_latched;
static volatile int64_t s_hb_ms;          /* 线程循环活性 */
static int s_err_streak;                  /* 链路错误连击(≥3 降级) */
static int64_t s_next_try_ms;             /* 重试退避 */

static dg_finger_mode_t s_mode = DG_FMODE_IDLE;
static ev_finger_mode_t s_cur;            /* 当前模式参数(uid/arg/pages/seq) */

/* 模式命令信箱(EV_FINGER_SET_MODE → 线程;单槽,新命令顶旧命令) */
static pthread_mutex_t s_cmd_mtx = PTHREAD_MUTEX_INITIALIZER;
static ev_finger_mode_t s_cmd;
static bool s_cmd_pending;

/* 序列中的回滚锚点:Store 成功后记录,取消/失败路径 DeletChar */
static int32_t s_stored_page = -1;

static event_subscription_t *s_sub;

static const fp_link_ops_t *s_link;       /* NULL = 板级 ops */
extern const fp_link_ops_t fp_link_uart;  /* fp_link_uart.c */

static void apply_cmd(void);              /* 前置:取消检查点内调用(定义在后) */

/* ---- 事件发布 ---- */

static void publish_state(bool fault)
{
    ev_sys_service_state_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.name, sizeof(ev.name), "%s", SVC_NAME);
    ev.state = fault ? REG_STATE_ERROR : REG_STATE_READY;
    EVENT_BUS_PUBLISH(EV_SYS_SERVICE_STATE, &ev);
}

static void degrade(void)
{
    s_ready = false;
    if (!s_fault_latched) {
        s_fault_latched = true;
        DG_LOGW(TAG, "模组失联,进降级(退避重试中)");
        publish_state(true);
        ev_finger_status_t ev = { .status = DG_FINGER_ERROR };
        EVENT_BUS_PUBLISH(EV_FINGER_STATUS, &ev);
    }
}

static void recover(void)
{
    s_err_streak = 0;
    if (s_fault_latched || !s_ready) {
        s_fault_latched = false;
        s_ready = true;
        DG_LOGI(TAG, "模组就绪(握手通过)");
        publish_state(false);
    } else {
        s_ready = true;
    }
}

bool fp_provider_ready(void)
{
    return s_ready;
}

int64_t fp_provider_heartbeat_ms(void)
{
    return s_hb_ms;
}

static void pub_status(int32_t st)
{
    ev_finger_status_t ev = { .status = st };
    EVENT_BUS_PUBLISH(EV_FINGER_STATUS, &ev);
}

static void pub_progress(int32_t kind, const char *uid, uint32_t seq,
                         int32_t percent, int32_t step)
{
    ev_enroll_progress_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.seq = seq;
    ev.percent = percent;
    ev.step = step;
    EVENT_BUS_PUBLISH(EV_ENROLL_PROGRESS, &ev);
}

static void pub_enroll_result(int32_t kind, const char *uid, uint32_t seq, int err)
{
    ev_enroll_result_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.seq = seq;
    ev.err = err;
    EVENT_BUS_PUBLISH(EV_ENROLL_RESULT, &ev);
}

/* ---- 链路:命令-应答引擎 ----
 * 应答帧可能跨包到达/与数据包粘包:字节进环形缓冲,持久解析器逐帧吐出。 */

#define FP_RX_RING_SZ 2048
static pthread_mutex_t s_rx_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_rx[FP_RX_RING_SZ];
static uint16_t s_rx_len;
static fp_parser_t s_parser;

static void rx_push(const uint8_t *d, size_t n)
{
    pthread_mutex_lock(&s_rx_mtx);
    if (n > sizeof(s_rx) - s_rx_len)
        n = sizeof(s_rx) - s_rx_len;      /* 满:丢新字节(校验和会暴露) */
    memcpy(s_rx + s_rx_len, d, n);
    s_rx_len = (uint16_t)(s_rx_len + n);
    pthread_mutex_unlock(&s_rx_mtx);
}

static void rx_pull(uint16_t n)
{
    pthread_mutex_lock(&s_rx_mtx);
    s_rx_len = (uint16_t)(s_rx_len - n);
    memmove(s_rx, s_rx + n, s_rx_len);
    pthread_mutex_unlock(&s_rx_mtx);
}

/** 等一帧(ACK/数据/结束);DG_OK / DG_ERR_TIMEOUT / DG_ERR_IO */
static int wait_frame(fp_frame_t *out, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        fp_frame_t f;
        size_t consumed = 0;
        pthread_mutex_lock(&s_rx_mtx);
        uint16_t len = s_rx_len;
        pthread_mutex_unlock(&s_rx_mtx);
        if (len > 0) {
            int r = fp_as608_parse(&s_parser, s_rx, len, &f, &consumed);
            if (consumed)
                rx_pull((uint16_t)consumed);
            if (r == 1) {
                *out = f;
                return DG_OK;
            }
            continue;                     /* -1 已自动复位续喂;0 需更多字节 */
        }
        int64_t remain = deadline - now_ms();
        if (remain <= 0)
            return DG_ERR_TIMEOUT;
        uint8_t tmp[128];
        size_t n = 0;
        int rc = s_link->recv(tmp, sizeof(tmp), remain > 20 ? 20 : (int)remain, &n);
        if (rc == DG_OK && n)
            rx_push(tmp, n);
        else if (rc != DG_OK && rc != DG_ERR_TIMEOUT)
            return DG_ERR_IO;
    }
}

/** 发指令并取应答帧;confirm 出参;DG_OK=拿到应答(确认码另判) */
static int cmd_xchg(const uint8_t *frame, size_t len, fp_frame_t *ack,
                    uint16_t *confirm, int timeout_ms)
{
    s_hb_ms = now_ms();
    if (s_link->send(frame, len) != DG_OK) {
        s_err_streak++;
        return DG_ERR_IO;
    }
    int rc = wait_frame(ack, timeout_ms);
    if (rc != DG_OK) {
        if (rc == DG_ERR_IO)
            s_err_streak++;
        return rc;
    }
    if (confirm)
        *confirm = fp_as608_ack_confirm(ack);
    return DG_OK;
}

/** 确认码 → 业务错误码(协议文档 §3 表) */
static int confirm_to_err(uint16_t c)
{
    switch (c) {
    case FP_ACK_OK:          return DG_OK;
    case FP_ACK_LIB_FULL:    return DG_ERR_FINGER_FULL;
    case FP_ACK_NOT_FOUND:   return DG_ERR_NOT_FOUND;
    case FP_ACK_NO_FINGER:
    case FP_ACK_ENROLL_FAIL:
    case FP_ACK_DRY_IMAGE:
    case FP_ACK_MERGE_FAIL:  return DG_ERR_MISMATCH;   /* 质量类:重按 */
    case FP_ACK_FLASH_ERR:   return DG_ERR_IO;
    default:                 return DG_ERR_INTERNAL;
    }
}

/* ---- 采集与序列 ---- */

#define FP_SEARCH_PAGES  999u              /* 协议 §5 冻结 golden:全库 0~999 */
#define FP_PAGE_COUNT    1000              /* PageID 空间 0~999(同上) */

/** 采集入 CharBuffer;quality_fail=true 表示"无手指/太干"类可重按失败 */
static int capture_to_buf(uint8_t buf_id, bool *quality_fail)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    *quality_fail = false;

    size_t n = fp_as608_get_image(b, sizeof(b));
    if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK)
        return DG_ERR_IO;
    if (c == FP_ACK_NO_FINGER || c == FP_ACK_DRY_IMAGE || c == FP_ACK_ENROLL_FAIL) {
        *quality_fail = true;
        return confirm_to_err(c);
    }
    if (c != FP_ACK_OK)
        return confirm_to_err(c);

    n = fp_as608_img2tz(b, sizeof(b), buf_id);
    if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK)
        return DG_ERR_IO;
    if (c == FP_ACK_NO_FINGER || c == FP_ACK_DRY_IMAGE || c == FP_ACK_ENROLL_FAIL) {
        *quality_fail = true;
        return confirm_to_err(c);
    }
    return confirm_to_err(c);
}

/** Search 全库查命中:0=未命中,1=命中(page/score 出参),负数=链路错 */
static int search_all(uint16_t *page, uint16_t *score)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    size_t n = fp_as608_search(b, sizeof(b), FP_A608_BUF1, 0, FP_SEARCH_PAGES);
    if (cmd_xchg(b, n, &ack, &c, 3000) != DG_OK)
        return DG_ERR_IO;
    if (c == FP_ACK_NOT_FOUND)
        return 0;
    if (c != FP_ACK_OK)
        return confirm_to_err(c);
    fp_as608_search_result(&ack, page, score);
    return 1;
}

/** 反查 DB → 发 1:N 事件(黑名单/角色过滤归 FSM,本层只给事实) */
static void pub_match_1n(uint16_t page, uint16_t score)
{
    ev_match_t m;
    memset(&m, 0, sizeof(m));
    char uid[DG_UID_LEN];
    if (db_finger_page_user(page, uid, sizeof(uid)) != DG_OK) {
        DG_LOGW(TAG, "PageID %u 无 DB 映射(孤儿模板,对账暴露)", page);
        m.matched = false;                /* fail-closed:查不到映射按陌生人 */
        EVENT_BUS_PUBLISH(EV_FINGER_MATCH_1N, &m);
        return;
    }
    user_rec_t rec;
    if (db_user_get(uid, &rec) != DG_OK) {
        m.matched = false;
        EVENT_BUS_PUBLISH(EV_FINGER_MATCH_1N, &m);
        return;
    }
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "%s", rec.user_id);
    snprintf(m.user_name, sizeof(m.user_name), "%s", rec.user_name);
    m.role = rec.role;
    m.auth_flags = rec.auth_flags;        /* "未开指纹"判定归 FSM(spec §7.2 表) */
    m.score_permille = score > 1000 ? 1000 : score;   /* 信息量展示,阈值在模组 */
    EVENT_BUS_PUBLISH(EV_FINGER_MATCH_1N, &m);
}

/* 1:N(普通/管理员):按压 → 采集 → Search → 发事件 */
static void seq_1n(void)
{
    bool qfail;
    int rc = capture_to_buf(FP_A608_BUF1, &qfail);
    if (rc == DG_ERR_IO)
        return;                           /* 链路错:下轮握手恢复 */
    if (qfail)
        return;                           /* 质量差:静默等下次按压(§5.1) */

    uint16_t page = 0, score = 0;
    rc = search_all(&page, &score);
    if (rc == DG_ERR_IO)
        return;
    if (rc == 1)
        pub_match_1n(page, score);
    else if (rc == 0) {
        ev_match_t m;
        memset(&m, 0, sizeof(m));         /* matched=false:陌生人(§5.1) */
        EVENT_BUS_PUBLISH(EV_FINGER_MATCH_1N, &m);
    }
}

/* 1:1(v_finger):对该用户 ≤3 枚逐一 LoadChar+Match(协议 §5 序列表) */
static void seq_verify_11(const char *uid)
{
    ev_match_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.user_id, sizeof(m.user_id), "%s", uid);

    user_rec_t rec;
    if (db_user_get(uid, &rec) == DG_OK) {
        snprintf(m.user_name, sizeof(m.user_name), "%s", rec.user_name);
        m.role = rec.role;
        m.auth_flags = rec.auth_flags;
    }

    int32_t pages[DG_FINGER_PAGES_MAX];
    uint32_t n = 0;
    if (db_finger_list_user(uid, pages, DG_FINGER_PAGES_MAX, &n) != DG_OK || n == 0) {
        m.matched = false;                /* 无指纹可验:不匹配(方式位应未开) */
        EVENT_BUS_PUBLISH(EV_FINGER_VERIFY_11, &m);
        return;
    }

    bool qfail;
    int rc = capture_to_buf(FP_A608_BUF1, &qfail);
    if (rc == DG_ERR_IO || qfail)
        return;                           /* 质量差静默,5s 超时由 FSM 收口 */

    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    for (uint32_t i = 0; i < n && !m.matched; i++) {
        size_t len = fp_as608_load_char(b, sizeof(b), FP_A608_BUF2,
                                        (uint16_t)pages[i]);
        if (cmd_xchg(b, len, &ack, &c, 2000) != DG_OK)
            return;
        if (c != FP_ACK_OK)
            continue;                     /* 孤儿页/坏模板:跳过该枚 */
        len = fp_as608_match(b, sizeof(b));
        if (cmd_xchg(b, len, &ack, &c, 2000) != DG_OK)
            return;
        if (c == FP_ACK_OK) {
            m.matched = true;
            m.score_permille = (ack.payload_len >= 3)
                                   ? (((uint16_t)ack.payload[1] << 8) | ack.payload[2])
                                   : 0;
            if (m.score_permille > 1000)
                m.score_permille = 1000;
        } else if (c != FP_ACK_NOT_FOUND && c != FP_ACK_NO_FINGER &&
                   c != FP_ACK_MERGE_FAIL) {
            s_err_streak++;               /* 非预期码:计数便于暴露 */
        }
    }
    EVENT_BUS_PUBLISH(EV_FINGER_VERIFY_11, &m);
}

/* ---- 录入(两次按压 + 同指校验 + 双向查重,§5.3) ---- */

static void enroll_rollback(void)
{
    if (s_stored_page < 0)
        return;
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    size_t n = fp_as608_delet_char(b, sizeof(b), (uint16_t)s_stored_page, 1);
    if (cmd_xchg(b, n, &ack, &c, 2000) == DG_OK && c == FP_ACK_OK)
        DG_LOGI(TAG, "录入回滚:PageID %d 已删", s_stored_page);
    else
        DG_LOGE(TAG, "录入回滚失败 PageID %d(孤儿,对账暴露)", s_stored_page);
    s_stored_page = -1;
}

/** 取消检查点:应用挂起命令(忙判只放行 IDLE);true = 已切出 ENROLL */
static bool enroll_canceled(void)
{
    apply_cmd();                          /* 本线程内调用,单写者安全 */
    return s_mode != DG_FMODE_ENROLL;
}

/** 等待按压(录入内);1=已按下,0=取消,-1=链路错。
 *  提示在等待前发(seq_enroll 各阶段入口),这里不再发——"请按指纹"
 *  提示在用户按下之后才出现就毫无意义 */
static int enroll_wait_press(void)
{
    int active = cfg_get()->fp_wak_active ? 1 : 0;
    int lvl;
    for (;;) {
        if (enroll_canceled())
            return 0;
        int rc = s_link->wak_wait(200, &lvl);
        if (rc == DG_ERR_TIMEOUT)
            continue;
        if (rc != DG_OK)
            return -1;
        if (lvl == active) {
            usleep(30 * 1000);            /* 消抖(§6) */
            int lv2 = -1;
            if (s_link->wak_level(&lv2) == DG_OK && lv2 != lvl)
                continue;
            return 1;
        }
    }
}

static void seq_enroll(void)
{
    const char *uid = s_cur.user_id;
    char uid_buf[DG_UID_LEN];
    snprintf(uid_buf, sizeof(uid_buf), "%s", uid);
    uid = uid_buf;

    /* 前置:单用户 ≤3 枚(FINGER_LIMIT)与模组库满(FULL)都不进采集(§6) */
    uint32_t cnt = 0;
    if (db_finger_count_user(uid, &cnt) != DG_OK) {
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_DB);
        s_mode = DG_FMODE_IDLE;
        return;
    }
    if (cnt >= DG_FINGER_PAGES_MAX) {
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_FINGER_LIMIT);
        s_mode = DG_FMODE_IDLE;
        return;
    }
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    size_t n = fp_as608_valid_num(b, sizeof(b));
    if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK || c != FP_ACK_OK) {
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_IO);
        s_mode = DG_FMODE_IDLE;
        return;
    }
    {
        /* payload = 确认码(1B) + 模板数(2B) */
        uint16_t valid = ((uint16_t)ack.payload[1] << 8) | ack.payload[2];
        if (valid >= FP_PAGE_COUNT) {
            pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_FINGER_FULL);
            s_mode = DG_FMODE_IDLE;
            return;
        }
    }

    s_stored_page = -1;
    pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 10, DG_ENROLL_FP_STEP_PRESS1);

    /* 按压① → 查重(尽早失败省一次按压,§5.3) */
    for (;;) {
        int wp = enroll_wait_press();
        if (wp == 0)
            goto cancel;
        if (wp < 0)
            goto link_err;
        pub_status(DG_FINGER_PRESSED);

        bool qfail;
        int rc = capture_to_buf(FP_A608_BUF1, &qfail);
        pub_status(DG_FINGER_RELEASED);
        if (rc == DG_ERR_IO)
            goto link_err;
        if (qfail) {
            /* 采集失败提示重按,不占进度(§6);仍等按压① */
            pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 10,
                         DG_ENROLL_FP_STEP_PRESS1);
            continue;
        }

        uint16_t page = 0, score = 0;
        rc = search_all(&page, &score);
        if (rc == DG_ERR_IO)
            goto link_err;
        if (rc == 1) {
            /* 命中(他人或与自己已有枚重复)→ DUP_FINGER,文案统一(§9) */
            char owner[DG_UID_LEN];
            if (db_finger_page_user(page, owner, sizeof(owner)) == DG_OK)
                DG_LOGI(TAG, "录入查重命中:PageID %u 属 %s", page, owner);
            pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_DUP_FINGER);
            s_mode = DG_FMODE_IDLE;
            return;
        }
        if (rc == 0)
            break;
        goto link_err;                    /* 非预期确认码 */
    }
    pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 50, DG_ENROLL_FP_STEP_PRESS2);

    /* 按压② → 同指校验 → 合成;不一致/质量差回 ② 重采 */
    for (;;) {
        int wp = enroll_wait_press();
        if (wp == 0)
            goto cancel;
        if (wp < 0)
            goto link_err;
        pub_status(DG_FINGER_PRESSED);

        bool qfail;
        int rc = capture_to_buf(FP_A608_BUF2, &qfail);
        if (rc == DG_ERR_IO) {
            pub_status(DG_FINGER_RELEASED);
            goto link_err;
        }
        if (qfail) {
            pub_status(DG_FINGER_RELEASED);
            /* 重按②,仍提示"再次按压" */
            pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 50,
                         DG_ENROLL_FP_STEP_PRESS2);
            continue;
        }

        n = fp_as608_match(b, sizeof(b));
        rc = cmd_xchg(b, n, &ack, &c, 2000);
        pub_status(DG_FINGER_RELEASED);
        if (rc == DG_ERR_IO)
            goto link_err;
        if (c != FP_ACK_OK) {
            /* 两次不同手指/差异过大:明确文案重采(§5.3);B1 仍有效 */
            pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 50,
                         DG_ENROLL_FP_STEP_RETRY2);
            continue;
        }

        n = fp_as608_reg_model(b, sizeof(b));
        rc = cmd_xchg(b, n, &ack, &c, 2000);
        if (rc == DG_ERR_IO)
            goto link_err;
        if (c != FP_ACK_OK) {             /* 合成失败兜底:回②重采(§6) */
            pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 50,
                         DG_ENROLL_FP_STEP_RETRY2);
            continue;
        }
        break;
    }

    /* PageID 分配 + Store + UpChar 落库(§5 序列:Store 后直接 UpChar B1) */
    int32_t page = -1;
    if (db_finger_alloc_page(&page) != DG_OK || page >= FP_PAGE_COUNT) {
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_FINGER_FULL);
        s_mode = DG_FMODE_IDLE;
        return;
    }
    n = fp_as608_store(b, sizeof(b), FP_A608_BUF1, (uint16_t)page);
    if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK)
        goto link_err;
    if (c != FP_ACK_OK) {
        enroll_rollback();
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, confirm_to_err(c));
        s_mode = DG_FMODE_IDLE;
        return;
    }
    s_stored_page = (int32_t)page;

    n = fp_as608_build_cmd(b, sizeof(b), 0x08, (const uint8_t[]){FP_A608_BUF1}, 1);
    if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK)
        goto link_err;
    if (c != FP_ACK_OK) {
        enroll_rollback();
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, confirm_to_err(c));
        s_mode = DG_FMODE_IDLE;
        return;
    }
    /* 数据包(0x02,512B 特征)+ 结束包(0x08) */
    fp_frame_t data;
    if (wait_frame(&data, 3000) != DG_OK || data.type != FP_A608_TYPE_DATA) {
        enroll_rollback();
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_IO);
        s_mode = DG_FMODE_IDLE;
        return;
    }
    fp_frame_t end;
    if (wait_frame(&end, 2000) != DG_OK) {
        /* 数据已到手、结束包缺席:按协议容忍(部分固件省略),不回滚 */
    }

    if (db_finger_add(uid, page, data.payload, data.payload_len) != DG_OK) {
        enroll_rollback();
        pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_DB);
        s_mode = DG_FMODE_IDLE;
        return;
    }
    s_stored_page = -1;                   /* DB 已落,回滚责任移交 DB 侧 */

    /* 首枚指纹把方式位打开(录入路径写位,方式选择自然列出,spec-auth §4.2) */
    user_rec_t rec;
    if (db_user_get(uid, &rec) == DG_OK && !(rec.auth_flags & DG_AUTH_FINGER)) {
        rec.auth_flags |= DG_AUTH_FINGER;
        if (db_user_update(&rec) != DG_OK)
            DG_LOGW(TAG, "auth_flags 写指纹位失败(%s):方式选择暂不显示", uid);
    }

    DG_LOGI(TAG, "录入完成 %s PageID %ld(%uB 副本)", uid, (long)page,
            (unsigned)data.payload_len);
    pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_OK);
    s_mode = DG_FMODE_IDLE;
    return;

link_err:
    enroll_rollback();
    pub_enroll_result(DG_ENROLL_FINGER, uid, s_cur.seq, DG_ERR_IO);
    s_mode = DG_FMODE_IDLE;
    return;
cancel:
    enroll_rollback();
    DG_LOGI(TAG, "录入取消(%s),已回滚未落库模板", uid);
    s_mode = DG_FMODE_IDLE;
}

/* 删单枚:先模组后 DB 行(模块失败则行保留,两侧一致,报错给 UI) */
static void seq_del_one(void)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    size_t n = fp_as608_delet_char(b, sizeof(b), (uint16_t)s_cur.arg, 1);
    int err;
    if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK) {
        err = DG_ERR_IO;
    } else if (c != FP_ACK_OK) {
        err = confirm_to_err(c);
    } else {
        err = db_finger_del(s_cur.user_id, s_cur.arg);
        uint32_t cnt = 0;
        if (err == DG_OK && db_finger_count_user(s_cur.user_id, &cnt) == DG_OK &&
            cnt == 0) {
            user_rec_t rec;               /* 最后一枚:方式位回收(spec §4.2) */
            if (db_user_get(s_cur.user_id, &rec) == DG_OK &&
                (rec.auth_flags & DG_AUTH_FINGER)) {
                rec.auth_flags &= ~(uint32_t)DG_AUTH_FINGER;
                if (db_user_update(&rec) != DG_OK)
                    DG_LOGW(TAG, "auth_flags 清指纹位失败(%s)", s_cur.user_id);
            }
        }
    }
    pub_enroll_result(DG_ENROLL_FINGER_DEL, s_cur.user_id, s_cur.seq, err);
    s_mode = DG_FMODE_IDLE;
}

/* 删用户:逐枚 DeletChar(失败留痕不阻塞,enroll_service 照常删 DB) */
static void seq_del_user_pages(void)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    for (int i = 0; i < s_cur.page_cnt; i++) {
        size_t n = fp_as608_delet_char(b, sizeof(b), s_cur.pages[i], 1);
        if (cmd_xchg(b, n, &ack, &c, 2000) != DG_OK || c != FP_ACK_OK)
            DG_LOGW(TAG, "删用户模板 PageID %u 失败(c=%u,孤儿由对账暴露)",
                    s_cur.pages[i], c);
        s_hb_ms = now_ms();
    }
    s_mode = DG_FMODE_IDLE;
}

/* ---- 按压沿与释放 ---- */

static bool debounce_press(int lvl)
{
    usleep(30 * 1000);
    int lv2 = -1;
    if (s_link->wak_level(&lv2) != DG_OK)
        return false;
    return lv2 == lvl;
}

static void wait_release(void)
{
    int active = cfg_get()->fp_wak_active ? 1 : 0;
    int lvl;
    for (;;) {
        int rc = s_link->wak_wait(200, &lvl);
        if (rc == DG_OK && lvl != active) {
            if (debounce_press(lvl))
                break;
            continue;
        }
        if (rc != DG_OK && rc != DG_ERR_TIMEOUT)
            return;                       /* 链路坏:上层降级兜底 */
    }
    pub_status(DG_FINGER_RELEASED);
}

/* ---- 模式命令 ---- */

static void apply_cmd(void)
{
    pthread_mutex_lock(&s_cmd_mtx);
    if (!s_cmd_pending) {
        pthread_mutex_unlock(&s_cmd_mtx);
        return;
    }
    ev_finger_mode_t cmd = s_cmd;
    s_cmd_pending = false;
    pthread_mutex_unlock(&s_cmd_mtx);

    /* 忙于录入/删除序列时只认 IDLE(取消);其余等序列自然结束(事件注) */
    bool busy = (s_mode == DG_FMODE_ENROLL || s_mode == DG_FMODE_FINGER_DEL ||
                 s_mode == DG_FMODE_DELETE_USER);
    if (busy && cmd.mode != DG_FMODE_IDLE) {
        DG_LOGD(TAG, "序列进行中,忽略模式 %d", cmd.mode);
        return;
    }
    s_mode = (dg_finger_mode_t)cmd.mode;
    s_cur = cmd;
}

static int on_set_mode(const event_t *e, void *ud)
{
    (void)ud;
    pthread_mutex_lock(&s_cmd_mtx);
    s_cmd = *(const ev_finger_mode_t *)e->data;
    s_cmd_pending = true;
    pthread_mutex_unlock(&s_cmd_mtx);
    return 0;
}

/* ---- 链路就绪(打开 + 握手 VerifyPSW) ---- */

static bool ensure_link(void)
{
    if (s_ready)
        return true;
    int64_t now = now_ms();
    if (now < s_next_try_ms)
        return false;
    s_next_try_ms = now + 2000;           /* 退避 2s(协议 §5.4) */

    const fp_link_ops_t *link = s_link ? s_link : &fp_link_uart;
    if (link->open() != DG_OK) {
        if (++s_err_streak >= 3)
            degrade();
        return false;
    }
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;   /* 失败路径也打日志:确认码给"无效"哨兵 */
    size_t n = fp_as608_verify_psw(b, sizeof(b), 0);   /* 出厂口令 0 */
    if (cmd_xchg(b, n, &ack, &c, 1000) != DG_OK || c != FP_ACK_OK) {
        DG_LOGW(TAG, "握手失败(confirm=%u),重试", c);
        link->close();
        if (++s_err_streak >= 3)
            degrade();
        return false;
    }
    recover();
    return true;
}

/* ---- 线程主循环 ---- */

static void *provider_thread(void *arg)
{
    (void)arg;
    s_hb_ms = now_ms();

    while (s_running) {
        s_hb_ms = now_ms();
        ensure_link();
        apply_cmd();
        if (!s_ready) {
            struct timespec ts = { 0, 200 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }

        int active = cfg_get()->fp_wak_active ? 1 : 0;
        switch (s_mode) {
        case DG_FMODE_ENROLL:
            seq_enroll();
            break;
        case DG_FMODE_FINGER_DEL:
            seq_del_one();
            break;
        case DG_FMODE_DELETE_USER:
            seq_del_user_pages();
            break;
        case DG_FMODE_VERIFY_11: {
            int lvl;
            int rc = s_link->wak_wait(200, &lvl);
            if (rc == DG_OK && lvl == active && debounce_press(lvl)) {
                pub_status(DG_FINGER_PRESSED);
                seq_verify_11(s_cur.user_id);
                s_hb_ms = now_ms();
                wait_release();
            } else if (rc != DG_OK && rc != DG_ERR_TIMEOUT) {
                degrade();
            }
            break;
        }
        case DG_FMODE_SCAN_1N: {
            int lvl;
            int rc = s_link->wak_wait(200, &lvl);
            if (rc == DG_OK && lvl == active && debounce_press(lvl)) {
                pub_status(DG_FINGER_PRESSED);
                seq_1n();
                s_hb_ms = now_ms();
                wait_release();
            } else if (rc != DG_OK && rc != DG_ERR_TIMEOUT) {
                degrade();
            }
            break;
        }
        default: {                        /* IDLE:只记状态事件,不做模组业务 */
            int lvl;
            int rc = s_link->wak_wait(200, &lvl);
            if (rc == DG_OK && lvl == active && debounce_press(lvl)) {
                pub_status(DG_FINGER_PRESSED);
                wait_release();
            } else if (rc != DG_OK && rc != DG_ERR_TIMEOUT) {
                degrade();
            }
            break;
        }
        }
    }
    return NULL;
}

void fp_provider_set_link_ops(const fp_link_ops_t *ops)
{
    s_link = ops;
}

int fp_provider_start(void)
{
    if (s_running)
        return DG_OK;
    s_running = true;
    fp_as608_parser_init(&s_parser);
    s_sub = event_bus_subscribe(EV_FINGER_SET_MODE, on_set_mode, NULL);
    if (pthread_create(&s_tid, NULL, provider_thread, NULL) == 0) {
        s_tid_created = true;
        DG_LOGI(TAG, "fp_provider 启动(dev=%s baud=%d wak=%d)",
                cfg_get()->fp_uart_dev, cfg_get()->fp_baud, cfg_get()->fp_wak_gpio);
        return DG_OK;
    }
    s_running = false;
    event_bus_unsubscribe(s_sub);
    s_sub = NULL;
    DG_LOGE(TAG, "provider 线程创建失败");
    return DG_ERR_INTERNAL;
}

void fp_provider_stop(void)
{
    if (!s_running)
        return;
    s_running = false;
    if (s_tid_created) {
        pthread_join(s_tid, NULL);
        s_tid_created = false;
    }
    event_bus_unsubscribe(s_sub);
    s_sub = NULL;
    s_ready = false;
}
