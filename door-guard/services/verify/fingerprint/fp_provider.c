/*
 * fp_provider.c — 指纹业务线程实现(FINGERPRINT_AS608.md §5 业务流程语义)
 *
 * 指令序列帧级依据:docs/tech/FINGERPRINT_PROTOCOL.md v1(§5 序列表)。
 * 一次按压一次判定:RELEASED 前不再触发新流程(§6 边界);录入是两次按压,
 * 查重插在按压①(采到即提示抬手、确认释放)之后,同指校验在按压②后,
 * 按压②采到即发 PROCESS(UI 停「5s 无按压」计时),取消/退出回滚已 Store 模板。
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
#include <stdatomic.h>
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
static _Atomic int64_t s_hb_ms;          /* 线程循环活性(看门狗跨线程读,C11 原子;
                                             * 与 netcore 心跳同风格,2026-10-04 统一) */
static int s_err_streak;                  /* 链路错误连击(≥3 降级) */
static int64_t s_next_try_ms;             /* 重试退避 */

static dg_finger_mode_t s_mode = DG_FMODE_IDLE;
static ev_finger_mode_t s_cur;            /* 当前模式参数(uid/arg/pages/seq) */

/* ---- WAK"按下"电平 ----
 * cfg finger.wak_active_level:-1=自动(默认)——启动时采样静息电平,
 * 按下=反相,消除接线极性这个板上未验收项;0/1=强制(产线已知极性可
 * 写 default.json 钉死)。自动档经 wak_calibrate 在线程启动时定一次 */
static int  s_wak_active = 1;             /* 生效"按下"电平(0/1) */
static bool s_wak_ready;
static int  s_edge_miss;                  /* 连续边沿超时计数(轮询兜底节奏) */

static int wait_press_once(int active);   /* 前置:定义在"按压沿与释放"段 */
static void wait_release(void);           /* 前置:LIFT 确认在录入序列先于定义使用 */

static int wak_active_level(void)
{
    return s_wak_ready ? s_wak_active
                       : (cfg_get()->fp_wak_active >= 0 ? cfg_get()->fp_wak_active : 1);
}

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

/* 先过一次边沿等待触发 gpio_hal 的 export/edge 装配(直读依赖已导出),
 * 再采样 1.2s 取众数为静息电平;采样全失败(引脚没接/没导出)沿用保守默认 */
static void wak_calibrate(void)
{
    if (cfg_get()->fp_wak_active >= 0) {
        s_wak_active = cfg_get()->fp_wak_active;
        s_wak_ready = true;
        DG_LOGI(TAG, "WAK 极性:配置强制 active=%d", s_wak_active);
        return;
    }
    int lv;
    (void)s_link->wak_wait(50, &lv);      /* 装配导出,结果不看 */
    int cnt[2] = { 0, 0 };
    int samples = 0;
    for (int i = 0; i < 12; i++) {
        if (s_link->wak_level(&lv) == DG_OK && (lv == 0 || lv == 1)) {
            cnt[lv]++;
            samples++;
        }
        usleep(100 * 1000);
    }
    if (samples == 0) {
        s_wak_active = 1;                 /* 引脚读不到:退保守默认,链路错走降级 */
        s_wak_ready = true;
        DG_LOGW(TAG, "WAK 电平采样失败(引脚未接/未导出?),极性按默认 active=1");
        return;
    }
    const int rest = cnt[0] >= cnt[1] ? 0 : 1;
    s_wak_active = !rest;
    s_wak_ready = true;
    DG_LOGI(TAG, "WAK 极性自校准:静息=%d 按下=%d(采样 %d:%d)", rest,
            s_wak_active, cnt[0], cnt[1]);
}

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
    return atomic_load(&s_hb_ms);
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

/** 等一帧(ACK/数据/结束);DG_OK / DG_ERR_TIMEOUT / DG_ERR_IO。
 *  时限用 MONOTONIC:校时步进(REALTIME 前跳)会把 remain 打成负数,
 *  等待中的指令应答全体秒超时,录入/验证当场报链路错 */
static int wait_frame(fp_frame_t *out, int timeout_ms)
{
    int64_t deadline = now_mono_ms() + timeout_ms;
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
        int64_t remain = deadline - now_mono_ms();
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
    atomic_store(&s_hb_ms, now_mono_ms());
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

/** 验证路径采集:按压未松期间立即重采(最多 3 次)。
 *  WAK 边沿在指腹刚触到传感器就翻转,首拍常赶在手指完全贴稳之前,
 *  NO_FINGER 即白丢一次按压、用户必须松手重按——「按了没反应」的
 *  主观迟钝来源(2026-10-04)。手指仍在 → 隔 150ms 再拍;已松开/链路错
 *  → 原样上抛(等下次按压/降级)。 */
static int capture_retry(uint8_t buf_id, bool *quality_fail)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        atomic_store(&s_hb_ms, now_mono_ms());           /* 重试循环也是活性,不等外部输入 */
        int rc = capture_to_buf(buf_id, quality_fail);
        if (rc != DG_ERR_MISMATCH)
            return rc;                     /* 成功或链路错:不重试 */
        int lv = -1;
        if (s_link->wak_level(&lv) != DG_OK || lv != wak_active_level())
            return rc;                     /* 已松开:再拍也是 NO_FINGER */
        usleep(150 * 1000);
    }
    return DG_ERR_MISMATCH;                /* 贴着仍拍不出:交上层按质量差处理 */
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
    int rc = capture_retry(FP_A608_BUF1, &qfail);
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
    int rc = capture_retry(FP_A608_BUF1, &qfail);
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
    for (;;) {
        if (enroll_canceled())
            return 0;
        int press = wait_press_once(wak_active_level());
        if (press < 0)
            return -1;
        if (press)
            return 1;
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
    DG_LOGI(TAG, "录入开始 %s(已录 %u/%d,WAK 按下=%d)", uid, cnt,
            DG_FINGER_PAGES_MAX, wak_active_level());
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

    /* 按压① → 提示抬手 → 查重(尽早失败省一次按压,§5.3) */
    for (;;) {
        int wp = enroll_wait_press();
        if (wp == 0)
            goto cancel;
        if (wp < 0)
            goto link_err;
        pub_status(DG_FINGER_PRESSED);

        bool qfail;
        int rc = capture_retry(FP_A608_BUF1, &qfail);
        pub_status(DG_FINGER_RELEASED);
        if (rc == DG_ERR_IO)
            goto link_err;
        if (qfail) {
            /* 采集失败不占进度,明确提示重按(§6);仍等按压① */
            pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 10,
                         DG_ENROLL_FP_STEP_QUALITY);
            continue;
        }

        /* 先提示放开手指(2026-10-04 用户口径):查重耗时与抬手动作重叠,
         * 提示在查重前发;确认释放后才提示第二按——否则「手指还按着却
         * 提示请再按一次」自相矛盾,第二按边沿也要等释放后才可能出现 */
        pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 30, DG_ENROLL_FP_STEP_LIFT);
        wait_release();
        if (enroll_canceled())            /* 等释放期间的取消检查点 */
            goto cancel;

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
        int rc = capture_retry(FP_A608_BUF2, &qfail);
        if (rc == DG_ERR_IO) {
            pub_status(DG_FINGER_RELEASED);
            goto link_err;
        }
        if (qfail) {
            pub_status(DG_FINGER_RELEASED);
            /* 采集失败不占进度,明确提示重按;仍等按压② */
            pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 50,
                         DG_ENROLL_FP_STEP_QUALITY);
            continue;
        }

        /* 按压②已采到:后续 Match/RegModel/Store/UpChar 不再需要输入,
         * PROCESS 让 UI 停「5s 无按压」计时器——处理尾巴可达数秒(UpChar
         * 降级等待 3s+),计时不停会先弹「已退出录入」再弹真终态 = 录入
         * 明明成功却先看到失败窗(2026-10-04 板上实测反馈的根因) */
        pub_progress(DG_ENROLL_FINGER, uid, s_cur.seq, 70,
                     DG_ENROLL_FP_STEP_PROCESS);

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

    /* UpChar 读回 512B 副本:验证主存储在模组 flash(Search/Match 都打它),
     * DB 向量仅备份/换模组回灌用——本步失败不推翻已成功的 Store,录入照常
     * 完成(降级:无副本,对账/回灌受损,WARN 留痕)。真机首次录入
     * (2026-10-03)本步必失败:数据/结束包帧式未定,协议文档 §6 待勾,
     * 旧实现回滚整次录入 = 用户白按两次还报"设备通讯异常" */
    const uint8_t *vec = NULL;
    size_t vec_len = 0;
    size_t n_up = fp_as608_build_cmd(b, sizeof(b), 0x08,
                                     (const uint8_t[]){FP_A608_BUF1}, 1);
    if (cmd_xchg(b, n_up, &ack, &c, 2000) == DG_OK && c == FP_ACK_OK) {
        fp_frame_t data;
        if (wait_frame(&data, 3000) == DG_OK && data.type == FP_A608_TYPE_DATA &&
            data.payload_len > 0) {
            vec = data.payload;
            vec_len = data.payload_len;
            fp_frame_t end;                   /* 结束包缺席容忍(部分固件省略) */
            (void)wait_frame(&end, 1000);
        } else {
            /* 排空残包(类型不符帧/结束包残片),别污染下一条指令的应答 */
            fp_frame_t t;
            while (wait_frame(&t, 200) == DG_OK) {
            }
            DG_LOGW(TAG, "UpChar 数据包未取得(超时/类型不符),无副本,录入继续");
        }
    } else {
        DG_LOGW(TAG, "UpChar 失败(确认码 %s),无副本,录入继续",
                fp_as608_confirm_name(c));
    }

    if (db_finger_add(uid, page, vec, vec_len) != DG_OK) {
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

    DG_LOGI(TAG, "录入完成 %s PageID %ld(%s)", uid, (long)page,
            vec_len ? "有副本" : "无副本");
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
        atomic_store(&s_hb_ms, now_mono_ms());
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

/* 一次按压检测:边沿为主,连续约 1s 边沿超时后补一次电平直读兜底。
 * sysfs 边沿依赖内核 edge 事件,极性配置错/边沿丢失/引脚悬空读数恒静息
 * 时,纯边沿等待 = 永远等不到按压(2026-10-03 板上"无法录入"主嫌疑);
 * 电平直读不依赖事件,≤1s 即能见到按下。1=按下,0=无,-1=链路错 */
static int wait_press_once(int active)
{
    /* 等按压 = 线程活着在等输入,不是挂死:必须按步刷心跳。否则录入等
     * 按压超过看门狗窗口(15s)即判 stale → registry_restart 对本服务
     * 只是空转(fp_provider_start 见 s_running 直接返回),下轮即 DISABLED
     * 粘死 → 验证按钮永远「指纹模块未就绪」(模组明明在线,2026-10-03) */
    atomic_store(&s_hb_ms, now_mono_ms());
    int lvl;
    int rc = s_link->wak_wait(200, &lvl);
    if (rc == DG_OK) {
        s_edge_miss = 0;
        if (lvl != active || !debounce_press(lvl))
            return 0;
        return 1;
    }
    if (rc != DG_ERR_TIMEOUT)
        return -1;                        /* 链路坏:上层降级兜底 */
    if (++s_edge_miss >= 5) {
        s_edge_miss = 0;
        int lv2;
        if (s_link->wak_level(&lv2) == DG_OK && lv2 == active &&
            debounce_press(lv2))
            return 1;
    }
    return 0;
}

/* 等释放期间也要按步应用模式命令:命令在信箱里等抬手 = 模式切换响应性
 * 丢失(2026-10-04 宿主测试暴露:上一流程残留的按压电平把 IDLE 分支带进
 * 本函数,后续 ENROLL 命令全被无视,整条录入链饿死)。模式一变立即让位,
 * 由主循环 switch 重新分派;正常释放照旧发 RELEASED 状态 */
static void wait_release(void)
{
    int active = wak_active_level();
    const dg_finger_mode_t mode0 = s_mode;
    int lvl;
    for (;;) {
        atomic_store(&s_hb_ms, now_mono_ms());          /* 等释放同 wait_press_once:按步刷心跳 */
        apply_cmd();
        if (s_mode != mode0)
            return;                       /* 命令切走:让位,不吞 RELEASED 状态 */
        int rc = s_link->wak_wait(200, &lvl);
        if (rc == DG_OK && lvl != active) {
            if (debounce_press(lvl))
                break;
            continue;
        }
        if (rc == DG_ERR_TIMEOUT) {
            int lv2;                      /* 边沿缺失兜底:直读电平判释放 */
            if (++s_edge_miss >= 5) {
                s_edge_miss = 0;
                if (s_link->wak_level(&lv2) == DG_OK && lv2 != active &&
                    debounce_press(lv2))
                    break;
            }
            continue;
        }
        if (rc != DG_OK)
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
    int64_t now = now_mono_ms();
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
    atomic_store(&s_hb_ms, now_mono_ms());
    wak_calibrate();

    while (s_running) {
        atomic_store(&s_hb_ms, now_mono_ms());
        ensure_link();
        apply_cmd();
        if (!s_ready) {
            struct timespec ts = { 0, 200 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }

        int active = wak_active_level();
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
            int press = wait_press_once(active);
            if (press < 0)
                degrade();
            else if (press) {
                pub_status(DG_FINGER_PRESSED);
                seq_verify_11(s_cur.user_id);
                atomic_store(&s_hb_ms, now_mono_ms());
                wait_release();
            }
            break;
        }
        case DG_FMODE_SCAN_1N: {
            int press = wait_press_once(active);
            if (press < 0)
                degrade();
            else if (press) {
                pub_status(DG_FINGER_PRESSED);
                seq_1n();
                atomic_store(&s_hb_ms, now_mono_ms());
                wait_release();
            }
            break;
        }
        default: {                        /* IDLE:只记状态事件,不做模组业务 */
            int press = wait_press_once(active);
            if (press < 0)
                degrade();
            else if (press) {
                pub_status(DG_FINGER_PRESSED);
                wait_release();
            }
            break;
        }
        }                                 /* switch */
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
    /* 未注入测试 ops 时在此解析默认板级链路:cmd_xchg/wait_frame 直接持
     * s_link-> 调用,置 NULL 会在板上首次握手指令时空指针崩(2026-10-03
     * core 定位;宿主测试因显式注入从未触发) */
    if (!s_link)
        s_link = &fp_link_uart;
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
