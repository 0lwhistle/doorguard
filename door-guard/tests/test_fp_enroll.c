/*
 * test_fp_enroll.c — 指纹 provider 端到端测试(假模组驱动,宿主无真硬件)
 *
 * 假模组(fp_link_ops 注入,FINGERPRINT_AS608 §7):收指令吐脚本化应答帧,
 * WAK 按压由测试信号注入。链路钉死:ENROLL_REQUEST → enroll(FINGER)
 * → EV_FINGER_SET_MODE(ENROLL) → provider 两次按压序列(查重/同指校验/
 * 合成/Store/UpChar)→ EV_ENROLL_RESULT + fingerprints 落库 + 方式位。
 *
 * 覆盖:happy path(落库+方式位+PageID 分配)/ 与他人重复 DUP_FINGER /
 * 同用户重复自己已有枚 DUP_FINGER / 单用户超 3 枚 FINGER_LIMIT /
 * 模组库满 FINGER_FULL / 两次不一致 RETRY2 重采后成功 / 按压①成像差
 * QUALITY 提示 / 取消回滚(无 Store 不留孤儿)/ DeletChar 单枚删除。
 */
#include "dg_test.h"
#include "cfg.h"
#include "err.h"
#include "enroll_service.h"
#include "event_bus.h"
#include "events.h"
#include "fp_as608.h"
#include "fp_provider.h"
#include "storage.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- 假模组:场景脚本 + 交互状态 ---- */

typedef struct {
    /* 场景(用例开始前设定) */
    uint16_t getimage_ack;      /* GetImage 应答确认码 */
    uint16_t img2tz_ack;        /* Img2Tz 应答(两个 buffer 共用场景) */
    int      search_hit;        /* 0=未命中 1=命中 */
    uint16_t search_page, search_score;
    uint16_t match_ack;         /* Match 应答(0=OK) */
    uint16_t regmodel_ack;
    uint16_t store_ack;
    uint16_t valid_count;       /* ValidTempleteNum 计数 */
    /* 交互观测 */
    atomic_int delet_calls;
    atomic_int delet_page;
    atomic_int store_page;
    /* WAK 电平模型:press_req 置 1 → 下次 wak_wait 返回按下沿;
     * release_req 同理释放沿;level 是消抖直读值 */
    atomic_int press_req, release_req, level;
    /* 应答字节流(锁内) */
    pthread_mutex_t mtx;
    uint8_t  rx[4096];
    size_t   rx_len;
} fake_t;

static fake_t F = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .level = 0,
};

static void fake_reset(void)
{
    pthread_mutex_lock(&F.mtx);
    F.rx_len = 0;
    pthread_mutex_unlock(&F.mtx);
    F.getimage_ack = 0;
    F.img2tz_ack = 0;
    F.search_hit = 0;
    F.search_page = 0;
    F.search_score = 500;
    F.match_ack = 0;
    F.regmodel_ack = 0;
    F.store_ack = 0;
    F.valid_count = 5;
    atomic_store(&F.delet_calls, 0);
    atomic_store(&F.delet_page, 0);
    atomic_store(&F.store_page, 0);
    atomic_store(&F.press_req, 0);
    atomic_store(&F.release_req, 0);
    atomic_store(&F.level, 0);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* 组应答帧:2026-10-03 真机勘误后口径 = 字面长度(ACK 确认码 1B,整帧 9+len) */
static void fake_push(const uint8_t *frame, size_t len)
{
    pthread_mutex_lock(&F.mtx);
    if (F.rx_len + len <= sizeof(F.rx)) {
        memcpy(F.rx + F.rx_len, frame, len);
        F.rx_len += len;
    }
    pthread_mutex_unlock(&F.mtx);
}

static size_t mk_ack(uint8_t *out, uint16_t confirm)
{
    memcpy(out, "\xEF\x01\xFF\xFF\xFF\xFF", 6);
    out[6] = FP_A608_TYPE_ACK;
    put_be16(out + 7, 3);                 /* 字面长度 = 确认码1B + 校验和2B */
    out[9] = (uint8_t)confirm;
    uint16_t sum = (uint16_t)(out[6] + out[7] + out[8] + out[9]);
    put_be16(out + 10, sum);
    return 12;
}

static size_t mk_ack_extra(uint8_t *out, uint16_t confirm, uint16_t a, uint16_t b)
{
    memcpy(out, "\xEF\x01\xFF\xFF\xFF\xFF", 6);
    out[6] = FP_A608_TYPE_ACK;
    put_be16(out + 7, 7);                 /* 确认码1B + a 2B + b 2B + 校验和2B */
    out[9] = (uint8_t)confirm;
    put_be16(out + 10, a);
    put_be16(out + 12, b);
    uint16_t sum = 0;
    for (int i = 6; i < 14; i++)
        sum = (uint16_t)(sum + out[i]);
    put_be16(out + 14, sum);
    return 16;
}

static void fake_respond(uint8_t cmd, const uint8_t *params)
{
    uint8_t b[FP_A608_FRAME_MAX];

    switch (cmd) {
    case 0x13:                            /* VerifyPSW */
    case 0x02:                            /* Img2Tz */
    case 0x05:                            /* RegModel */
    case 0x03:                            /* Match */
    case 0x06: {                          /* Store */
        uint16_t ack = 0;
        if (cmd == 0x02)
            ack = F.img2tz_ack;
        else if (cmd == 0x05)
            ack = F.regmodel_ack;
        else if (cmd == 0x03)
            ack = F.match_ack;
        else if (cmd == 0x06) {
            ack = F.store_ack;
            if (ack == FP_ACK_OK)
                atomic_store(&F.store_page, (int)((params[1] << 8) | params[2]));
        }
        size_t n = mk_ack(b, ack);
        fake_push(b, n);
        break;
    }
    case 0x01: {                          /* GetImage */
        size_t n = mk_ack(b, F.getimage_ack);
        fake_push(b, n);
        break;
    }
    case 0x04: {                          /* Search */
        if (F.search_hit) {
            size_t n = mk_ack_extra(b, FP_ACK_OK, F.search_page, F.search_score);
            fake_push(b, n);
        } else {
            size_t n = mk_ack(b, FP_ACK_NOT_FOUND);
            fake_push(b, n);
        }
        break;
    }
    case 0x08: {                          /* UpChar:ACK + 512B 数据包 + 结束包 */
        size_t n = mk_ack(b, FP_ACK_OK);
        fake_push(b, n);
        uint8_t data[FP_A608_PAYLOAD_MAX];
        for (int i = 0; i < 512; i++)
            data[i] = (uint8_t)(i * 7 + 3);
        memcpy(b, "\xEF\x01\xFF\xFF\xFF\xFF", 6);
        b[6] = FP_A608_TYPE_DATA;
        put_be16(b + 7, (uint16_t)(512 + 2));
        memcpy(b + 9, data, 512);
        uint16_t sum = (uint16_t)(b[6] + b[7] + b[8]);
        for (int i = 0; i < 512; i++)
            sum = (uint16_t)(sum + data[i]);
        put_be16(b + 9 + 512, sum);
        fake_push(b, 9 + 512 + 2);
        memcpy(b, "\xEF\x01\xFF\xFF\xFF\xFF", 6);
        b[6] = FP_A608_TYPE_END;
        put_be16(b + 7, 3);
        put_be16(b + 9, (uint16_t)(0x08 + 0x00 + 0x03));
        fake_push(b, 13);
        break;
    }
    case 0x0C: {                          /* DeletChar */
        atomic_fetch_add(&F.delet_calls, 1);
        atomic_store(&F.delet_page, (int)((params[0] << 8) | params[1]));
        size_t n = mk_ack(b, FP_ACK_OK);
        fake_push(b, n);
        break;
    }
    case 0x1D: {                          /* ValidTempleteNum */
        size_t n = mk_ack_extra(b, FP_ACK_OK, F.valid_count, 0);
        fake_push(b, n);
        break;
    }
    default:
        break;                            /* 未知指令静默:等待超时暴露 */
    }
}

/* ---- fp_link_ops 假实现 ---- */

static int fk_open(void)
{
    return 0;
}

static void fk_close(void)
{
}

static int fk_send(const uint8_t *data, size_t len)
{
    if (len < 10 || data[6] != FP_A608_TYPE_CMD)
        return -1;
    fake_respond(data[9], data + 10);
    return 0;
}

static int fk_recv(uint8_t *buf, size_t cap, int timeout_ms, size_t *out_len)
{
    *out_len = 0;
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        pthread_mutex_lock(&F.mtx);
        if (F.rx_len) {
            size_t n = F.rx_len < cap ? F.rx_len : cap;
            memcpy(buf, F.rx, n);
            memmove(F.rx, F.rx + n, F.rx_len - n);
            F.rx_len -= n;
            pthread_mutex_unlock(&F.mtx);
            *out_len = n;
            return 0;
        }
        pthread_mutex_unlock(&F.mtx);
        usleep(5000);
    }
    return DG_ERR_TIMEOUT;                /* 契约码:provider 区分"无帧"与链路错 */
}

static int fk_wak_wait(int timeout_ms, int *level)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (atomic_exchange(&F.press_req, 0)) {
            atomic_store(&F.level, 1);
            *level = 1;
            return 0;                     /* 按下沿 */
        }
        if (atomic_exchange(&F.release_req, 0)) {
            atomic_store(&F.level, 0);
            *level = 0;
            return 0;                     /* 释放沿 */
        }
        usleep(5000);
    }
    return DG_ERR_TIMEOUT;                /* 契约码:provider 区分"无沿"与链路错 */
}

static int fk_wak_level(int *level)
{
    *level = atomic_load(&F.level);
    return 0;
}

static const fp_link_ops_t FAKE_OPS = {
    .open = fk_open,
    .close = fk_close,
    .send = fk_send,
    .recv = fk_recv,
    .wak_wait = fk_wak_wait,
    .wak_level = fk_wak_level,
};

/* ---- 事件观测 ---- */

static atomic_int s_progress_cnt;
static atomic_int s_progress_step;        /* 最近一次进度 step */
static atomic_int s_result_finger_cnt;
static atomic_int s_result_finger_err;
static atomic_int s_result_del_cnt;
static atomic_int s_result_del_err;

static int on_progress(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_progress_t *p = (const ev_enroll_progress_t *)e->data;
    if (p->kind != DG_ENROLL_FINGER)
        return 0;
    atomic_store(&s_progress_step, p->step);
    atomic_fetch_add(&s_progress_cnt, 1);
    return 0;
}

static int on_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_result_t *r = (const ev_enroll_result_t *)e->data;
    if (r->kind == DG_ENROLL_FINGER) {
        atomic_store(&s_result_finger_err, r->err);
        atomic_fetch_add(&s_result_finger_cnt, 1);
    } else if (r->kind == DG_ENROLL_FINGER_DEL) {
        atomic_store(&s_result_del_err, r->err);
        atomic_fetch_add(&s_result_del_cnt, 1);
    }
    return 0;
}

static void wait_ge(volatile atomic_int *c, int want, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        if (atomic_load(c) >= want)
            return;
        usleep(5000);
    }
}

static void press(void)
{
    atomic_store(&F.press_req, 1);
}

static void publish_req(const char *uid, int32_t kind, int32_t arg)
{
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.arg = arg;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
}

static void make_user(const char *uid, const char *name)
{
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", uid);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", name);
    rec.role = DG_ROLE_NORMAL;
    rec.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    DG_CHECK(db_user_set_password(&rec, "abcd1234") == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
}

static void add_finger_row(const char *uid, int32_t page)
{
    uint8_t vec[64];
    memset(vec, 0xAB, sizeof(vec));
    DG_CHECK(db_finger_add(uid, page, vec, sizeof(vec)) == DG_OK);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);

    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/dg_fp_enroll_%d", (int)getpid());
    char cmd[320];   /* 两条 %s(dir 上限 127)不截断 */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    DG_CHECK(system(cmd) == 0);
    char db[192], key[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", dir);
    snprintf(key, sizeof(key), "%s/dg.key", dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    DG_CHECK(cfg_load(NULL, NULL) == DG_OK);    /* 纯默认(wak_active=-1 自动极性) */

    event_bus_subscribe(EV_ENROLL_PROGRESS, on_progress, NULL);
    event_bus_subscribe(EV_ENROLL_RESULT, on_result, NULL);
    DG_CHECK(enroll_service_start() == DG_OK);
    fp_provider_set_link_ops(&FAKE_OPS);
    DG_CHECK(fp_provider_start() == DG_OK);

    make_user("30001", "张三");

    /* 等 provider 就绪:线程启动先做 WAK 极性自校准(自动档 ~1.2s 采样)
     * 再握手,就绪上限放宽到 5s */
    for (int i = 0; i < 500 && !fp_provider_ready(); i++)
        usleep(10000);
    DG_CHECK(fp_provider_ready());

    printf("[P1] happy path:两次按压 → Store+UpChar → 落库+方式位\n");
    fake_reset();
    publish_req("30001", DG_ENROLL_FINGER, 0);
    wait_ge(&s_progress_cnt, 1, 3000);    /* PRESS1 提示(等待前发) */
    DG_CHECK(atomic_load(&s_progress_step) == DG_ENROLL_FP_STEP_PRESS1);
    press();
    wait_ge(&s_progress_cnt, 2, 5000);    /* PRESS2 提示(查重通过) */
    DG_CHECK(atomic_load(&s_progress_step) == DG_ENROLL_FP_STEP_PRESS2);
    press();
    wait_ge(&s_result_finger_cnt, 1, 5000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_OK);

    uint32_t cnt = 99;
    DG_CHECK(db_finger_count_user("30001", &cnt) == DG_OK && cnt == 1);
    int32_t pages[DG_FINGER_PAGES_MAX];
    uint32_t np = 0;
    DG_CHECK(db_finger_list_user("30001", pages, DG_FINGER_PAGES_MAX, &np) == DG_OK);
    DG_CHECK(np == 1 && pages[0] == atomic_load(&F.store_page));
    const int32_t p1_page = (int32_t)atomic_load(&F.store_page);
    user_rec_t rec;
    DG_CHECK(db_user_get("30001", &rec) == DG_OK);
    DG_CHECK(rec.auth_flags & DG_AUTH_FINGER);  /* 首枚指纹开方式位 */
    uint8_t vec[DG_FEATURE_MAX];
    size_t vlen = 0;
    DG_CHECK(db_finger_get_vec(pages[0], vec, sizeof(vec), &vlen) == DG_OK);
    DG_CHECK(vlen == 512 && vec[0] == 0x03);    /* UpChar 副本完整落库 */
    DG_CHECK(atomic_load(&F.delet_calls) == 0); /* 无回滚 */

    printf("[P2] 与他人重复:Search 命中 → DUP_FINGER,无 Store\n");
    fake_reset();
    add_finger_row("39999", 500);         /* 他人已占 PageID 500 */
    F.search_hit = 1;
    F.search_page = 500;
    publish_req("30001", DG_ENROLL_FINGER, 0);
    wait_ge(&s_progress_cnt, 3, 3000);
    press();
    wait_ge(&s_result_finger_cnt, 2, 5000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_ERR_DUP_FINGER);
    DG_CHECK(atomic_load(&F.store_page) != 500);

    printf("[P2b] 同用户重复:Search 命中自己已有页 → DUP_FINGER(同指不重录)\n");
    fake_reset();
    F.search_hit = 1;
    F.search_page = (uint16_t)p1_page;    /* P1 落库的 30001 自己的页 */
    publish_req("30001", DG_ENROLL_FINGER, 0);
    wait_ge(&s_progress_cnt, 4, 3000);
    press();
    wait_ge(&s_result_finger_cnt, 3, 5000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_ERR_DUP_FINGER);
    DG_CHECK(atomic_load(&F.delet_calls) == 0);   /* 未 Store 无需回滚 */
    cnt = 99;
    DG_CHECK(db_finger_count_user("30001", &cnt) == DG_OK && cnt == 1);

    printf("[P3] 单用户超 3 枚:FINGER_LIMIT,不进采集\n");
    fake_reset();
    make_user("30002", "李四");
    add_finger_row("30002", 600);
    add_finger_row("30002", 601);
    add_finger_row("30002", 602);
    int prog_before = atomic_load(&s_progress_cnt);
    publish_req("30002", DG_ENROLL_FINGER, 0);
    wait_ge(&s_result_finger_cnt, 4, 3000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_ERR_FINGER_LIMIT);
    DG_CHECK(atomic_load(&s_progress_cnt) == prog_before);  /* 相对断言:无新进度 = 没等按压 */

    printf("[P4] 模组库满:FINGER_FULL\n");
    fake_reset();
    F.valid_count = 1000;
    publish_req("30001", DG_ENROLL_FINGER, 0);
    wait_ge(&s_result_finger_cnt, 5, 3000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_ERR_FINGER_FULL);

    printf("[P5] 两次不一致:RETRY2 重采后成功\n");
    fake_reset();
    atomic_store(&s_progress_cnt, 0);
    publish_req("30001", DG_ENROLL_FINGER, 0);
    wait_ge(&s_progress_cnt, 1, 3000);    /* PRESS1 */
    press();                              /* 按压①(查重未命中) */
    wait_ge(&s_progress_cnt, 1 + 1, 5000);/* PRESS2 */
    F.match_ack = FP_ACK_MERGE_FAIL;      /* 按压②同指校验不过 */
    press();
    for (int i = 0; i < 1000 && atomic_load(&s_progress_step)
                                        != DG_ENROLL_FP_STEP_RETRY2; i++)
        usleep(5000);
    DG_CHECK(atomic_load(&s_progress_step) == DG_ENROLL_FP_STEP_RETRY2);
    F.match_ack = FP_ACK_OK;
    press();                              /* 重采② */
    wait_ge(&s_result_finger_cnt, 6, 5000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_OK);

    printf("[P5b] 按压①成像差:GetImage 无指纹 → QUALITY 提示,重按走完全程\n");
    fake_reset();
    atomic_store(&s_progress_cnt, 0);
    make_user("30003", "王五");
    F.getimage_ack = FP_ACK_NO_FINGER;
    publish_req("30003", DG_ENROLL_FINGER, 0);
    wait_ge(&s_progress_cnt, 1, 3000);    /* PRESS1 */
    press();                              /* 按压①:不成像 */
    for (int i = 0; i < 1000 && atomic_load(&s_progress_step)
                                        != DG_ENROLL_FP_STEP_QUALITY; i++)
        usleep(5000);
    DG_CHECK(atomic_load(&s_progress_step) == DG_ENROLL_FP_STEP_QUALITY);
    F.getimage_ack = 0;
    press();                              /* 重按:此后全部默认应答 */
    wait_ge(&s_progress_cnt, 3, 5000);    /* PRESS2 */
    press();
    wait_ge(&s_result_finger_cnt, 7, 5000);
    DG_CHECK(atomic_load(&s_result_finger_err) == DG_OK);
    cnt = 99;
    DG_CHECK(db_finger_count_user("30003", &cnt) == DG_OK && cnt == 1);

    printf("[P6] 取消:未 Store 即退出,无回滚无结果\n");
    fake_reset();
    atomic_store(&s_progress_cnt, 0);
    int result_before = atomic_load(&s_result_finger_cnt);
    publish_req("30001", DG_ENROLL_FINGER, 0);
    wait_ge(&s_progress_cnt, 1, 3000);    /* 进入录入(PRESS1 已发) */
    publish_req("30001", DG_ENROLL_FINGER_CANCEL, 0);
    usleep(300 * 1000);                   /* 取消生效(provider 200ms 粒度) */
    press();                              /* 迟到的按压不得再进流程 */
    usleep(500 * 1000);
    DG_CHECK(atomic_load(&s_result_finger_cnt) == result_before);
    DG_CHECK(atomic_load(&F.store_page) == 0);  /* 从未 Store */
    DG_CHECK(atomic_load(&F.delet_calls) == 0);
    /* 迟到按压已被 IDLE 分支消费:补释放沿,否则 provider 停在 wait_release,
     * fp_provider_stop 的 pthread_join 挂死(ctest TIMEOUT) */
    atomic_store(&F.release_req, 1);
    usleep(300 * 1000);

    printf("[P7] 逐枚删除:DeletChar + 删行 + 末枚收方式位\n");
    fake_reset();
    /* 30002 的 3 行 page=600/601/602;删 601 */
    publish_req("30002", DG_ENROLL_FINGER_DEL, 601);
    wait_ge(&s_result_del_cnt, 1, 3000);
    DG_CHECK(atomic_load(&s_result_del_err) == DG_OK);
    DG_CHECK(atomic_load(&F.delet_calls) >= 1);
    DG_CHECK(atomic_load(&F.delet_page) == 601);
    DG_CHECK(db_finger_count_user("30002", &cnt) == DG_OK && cnt == 2);

    /* 全部删光 → 方式位回收 */
    publish_req("30002", DG_ENROLL_FINGER_DEL, 600);
    wait_ge(&s_result_del_cnt, 2, 3000);
    publish_req("30002", DG_ENROLL_FINGER_DEL, 602);
    wait_ge(&s_result_del_cnt, 3, 3000);
    DG_CHECK(db_finger_count_user("30002", &cnt) == DG_OK && cnt == 0);
    DG_CHECK(db_user_get("30002", &rec) == DG_OK);
    DG_CHECK(!(rec.auth_flags & DG_AUTH_FINGER));
    DG_CHECK(rec.auth_flags & DG_AUTH_FACE);    /* 别误清其他位 */

    fp_provider_stop();
    enroll_service_stop();
    storage_deinit();
    DG_TEST_EXIT();
}
