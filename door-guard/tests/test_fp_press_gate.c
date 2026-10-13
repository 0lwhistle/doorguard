/*
 * test_fp_press_gate.c — 按压确认窗测试(假链路注入,宿主无真硬件)
 *
 * 用户口径(2026-10-13):短于 finger.press_hold_ms(默认 500ms)的 WAK
 * 有效保持都是假按压——克隆模组触摸感应前置(悬停/掠过即翻转)+ 干扰毛刺。
 * 假链路(fp_link_ops 注入,FINGERPRINT_AS608 §7)验证:
 *   1. 出厂默认即 500(用户拍板值钉进默认配置,防回归漂移)
 *   2. IDLE:短保持不出 PRESSED(待机即时亮屏不被误触发),长保持照常
 *   3. SCAN_1N:短保持不发起采集(GetImage 计数 0),长保持采集照常
 *   4. 配置置 0 → 钳 30ms 下限(只留电气消抖,不裸奔)
 */
#include "dg_test.h"
#include "cfg.h"
#include "err.h"
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
#include <unistd.h>

/* ---- 假链路:应答脚本 + WAK 电平模型 ---- */

typedef struct {
    atomic_int getimage_calls;            /* 观测:是否发起了采集 */
    /* WAK 电平模型:press_req 置 1 → 下次 wak_wait 返回按下沿(电平置 1);
     * release_req 同理释放沿(电平置 0);level 为电平直读值 */
    atomic_int press_req, release_req, level;
    pthread_mutex_t mtx;
    uint8_t  rx[1024];
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
    atomic_store(&F.getimage_calls, 0);
    atomic_store(&F.press_req, 0);
    atomic_store(&F.release_req, 0);
    atomic_store(&F.level, 0);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

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

/* ValidTempleteNum 应答:确认码1B + 模板数2B + 校验和2B */
static size_t mk_ack_count(uint8_t *out, uint16_t confirm, uint16_t count)
{
    memcpy(out, "\xEF\x01\xFF\xFF\xFF\xFF", 6);
    out[6] = FP_A608_TYPE_ACK;
    put_be16(out + 7, 5);
    out[9] = (uint8_t)confirm;
    put_be16(out + 10, count);
    uint16_t sum = 0;
    for (int i = 6; i < 12; i++)
        sum = (uint16_t)(sum + out[i]);
    put_be16(out + 12, sum);
    return 14;
}

static void fake_respond(uint8_t cmd)
{
    uint8_t b[FP_A608_FRAME_MAX];
    switch (cmd) {
    case 0x13:                            /* VerifyPSW:握手 */
        fake_push(b, mk_ack(b, FP_ACK_OK));
        break;
    case 0x01:                            /* GetImage:观测采集发起 */
        atomic_fetch_add(&F.getimage_calls, 1);
        fake_push(b, mk_ack(b, FP_ACK_OK));
        break;
    case 0x04:                            /* Search:全库空 → 未命中 */
        fake_push(b, mk_ack(b, FP_ACK_NOT_FOUND));
        break;
    case 0x1D:                            /* ValidTempleteNum:模组 0 枚 */
        fake_push(b, mk_ack_count(b, FP_ACK_OK, 0));
        break;
    default:                              /* Img2Tz/Match/…:一律成功 */
        fake_push(b, mk_ack(b, FP_ACK_OK));
        break;
    }
}

static int fk_open(void)
{
    return DG_OK;
}

static void fk_close(void)
{
}

static int fk_send(const uint8_t *data, size_t len)
{
    if (len >= 10)
        fake_respond(data[9]);            /* 指令码在 payload[0] = 偏移 9 */
    return DG_OK;
}

static int fk_recv(uint8_t *buf, size_t cap, int timeout_ms, size_t *out_len)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        pthread_mutex_lock(&F.mtx);
        if (F.rx_len > 0) {
            size_t n = F.rx_len < cap ? F.rx_len : cap;
            memcpy(buf, F.rx, n);
            memmove(F.rx, F.rx + n, F.rx_len - n);
            F.rx_len -= n;
            pthread_mutex_unlock(&F.mtx);
            *out_len = n;
            return DG_OK;
        }
        pthread_mutex_unlock(&F.mtx);
        usleep(5000);
    }
    return DG_ERR_TIMEOUT;
}

static int fk_wak_wait(int timeout_ms, int *level)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (atomic_exchange(&F.press_req, 0)) {
            atomic_store(&F.level, 1);
            *level = 1;
            return DG_OK;                 /* 按下沿 */
        }
        if (atomic_exchange(&F.release_req, 0)) {
            atomic_store(&F.level, 0);
            *level = 0;
            return DG_OK;                 /* 释放沿 */
        }
        usleep(5000);
    }
    return DG_ERR_TIMEOUT;
}

static int fk_wak_level(int *level)
{
    *level = atomic_load(&F.level);
    return DG_OK;
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

static atomic_int s_pressed_cnt;
static atomic_int s_released_cnt;

static int on_status(const event_t *e, void *ud)
{
    (void)ud;
    const ev_finger_status_t *st = (const ev_finger_status_t *)e->data;
    if (st->status == DG_FINGER_PRESSED)
        atomic_fetch_add(&s_pressed_cnt, 1);
    else if (st->status == DG_FINGER_RELEASED)
        atomic_fetch_add(&s_released_cnt, 1);
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

static void release(void)
{
    atomic_store(&F.release_req, 1);
}

/* 短保持场景:给按下沿,保持 hold_ms 后把电平拉回静息(手指掠过) */
static void short_press(int hold_ms)
{
    press();
    usleep(hold_ms * 1000);
    atomic_store(&F.level, 0);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);

    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/dg_fp_gate_%d", (int)getpid());
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    DG_CHECK(system(cmd) == 0);
    char db[192], key[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", dir);
    snprintf(key, sizeof(key), "%s/dg.key", dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    DG_CHECK(cfg_load(NULL, NULL) == DG_OK);

    /* [P0] 用户拍板值(2026-10-13:短于 500ms 都是假按压)钉在出厂默认 */
    DG_CHECK(cfg_get()->fp_press_hold_ms == 500);

    event_bus_subscribe(EV_FINGER_STATUS, on_status, NULL);
    fp_provider_set_link_ops(&FAKE_OPS);
    DG_CHECK(fp_provider_start() == DG_OK);

    /* 等就绪:线程启动先做 WAK 极性自校准(自动档 ~1.2s 采样)再握手 */
    for (int i = 0; i < 500 && !fp_provider_ready(); i++)
        usleep(10000);
    DG_CHECK(fp_provider_ready());
    usleep(300 * 1000);                   /* 开机一致性检查跑完再开始按压 */

    printf("[P1] IDLE:短保持(<确认窗)→ 无 PRESSED,待机亮屏不被误触发\n");
    fake_reset();
    short_press(100);
    usleep(900 * 1000);                   /* 给足确认窗超时 + 兜底轮询的余量 */
    DG_CHECK(atomic_load(&s_pressed_cnt) == 0);

    printf("[P2] IDLE:长保持(≥确认窗)→ PRESSED + RELEASED 照常\n");
    press();                              /* 边沿置电平 1,保持不放 */
    wait_ge(&s_pressed_cnt, 1, 3000);
    DG_CHECK(atomic_load(&s_pressed_cnt) == 1);
    release();
    wait_ge(&s_released_cnt, 1, 2000);
    DG_CHECK(atomic_load(&s_released_cnt) == 1);

    printf("[P3] SCAN_1N:短保持不发起采集,长保持采集照常\n");
    fake_reset();
    ev_finger_mode_t mode;
    memset(&mode, 0, sizeof(mode));
    mode.mode = DG_FMODE_SCAN_1N;
    mode.seq = 1;
    EVENT_BUS_PUBLISH(EV_FINGER_SET_MODE, &mode);
    usleep(200 * 1000);                   /* 主循环顶部 apply_cmd 生效 */
    short_press(100);
    usleep(900 * 1000);
    DG_CHECK(atomic_load(&F.getimage_calls) == 0);
    DG_CHECK(atomic_load(&s_pressed_cnt) == 1);   /* 沿用 P2,无新增 */
    press();
    wait_ge(&s_pressed_cnt, 2, 3000);
    wait_ge(&F.getimage_calls, 1, 3000);
    DG_CHECK(atomic_load(&F.getimage_calls) >= 1);
    release();
    wait_ge(&s_released_cnt, 2, 2000);

    printf("[P4] 配置置 0 → 钳 30ms 下限(只留电气消抖,不裸奔)\n");
    /* cfg_load 两路径缺一即走纯内置默认分支(不读文件),default/cur 都要给 */
    char jpath[160], cpath[160];
    snprintf(jpath, sizeof(jpath), "%s/default.json", dir);
    snprintf(cpath, sizeof(cpath), "%s/cur.json", dir);
    FILE *fp = fopen(jpath, "w");
    DG_CHECK(fp != NULL);
    fputs("{\"finger\":{\"press_hold_ms\":0}}", fp);
    fclose(fp);
    fp = fopen(cpath, "w");
    DG_CHECK(fp != NULL);
    fputs("{}", fp);
    fclose(fp);
    DG_CHECK(cfg_load(jpath, cpath) == DG_OK);
    DG_CHECK(cfg_get()->fp_press_hold_ms == 0);
    press();                              /* 30ms 窗:立即按住即过 */
    wait_ge(&s_pressed_cnt, 3, 2000);
    DG_CHECK(atomic_load(&s_pressed_cnt) == 3);
    release();
    wait_ge(&s_released_cnt, 3, 2000);

    fp_provider_stop();
    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    DG_CHECK(system(cmd) == 0);
    DG_TEST_EXIT();
}
