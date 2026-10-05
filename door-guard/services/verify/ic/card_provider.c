/*
 * card_provider.c — IC 读卡服务实现(契约见 card_provider.h / ICCARD_PROTOCOL.md)
 */
#include "card_provider.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "iccard_hal.h"
#include "registry.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "[CARDCARD]";

static const char *SVC_NAME = "iccard";   /* EV_SYS_SERVICE_STATE 注册名 */

static volatile bool s_running;
static pthread_t s_tid;
static bool s_tid_created;
static int s_fd = -1;
static bool s_ready;                      /* 节点在位且读通 */
static bool s_fault_latched;              /* 已广播 ERROR(恢复时回 READY 一次) */
static volatile bool s_flush_req;         /* 总线线程置位,线程内执行(防 fd 竞态) */

static event_subscription_t *s_sub;

/* 降级广播 latch:同一故障只发一次 ERROR,恢复发 READY(对齐 relay latch) */
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
    if (!s_fault_latched) {
        s_fault_latched = true;
        s_ready = false;
        DG_LOGW(TAG, "读卡器失联,进降级(退避重试中)");
        publish_state(true);
    }
}

static void recover(void)
{
    if (s_fault_latched || !s_ready) {
        s_fault_latched = false;
        s_ready = true;
        DG_LOGI(TAG, "读卡器就绪");
        publish_state(false);
    }
}

bool card_provider_ready(void)
{
    return s_ready;
}

bool card_dedup_allow(card_dedup_t *st, const char *no, int64_t now_ms,
                      int32_t window_ms)
{
    if (!st || !no || !no[0])
        return false;
    if (st->last_no[0] && strcmp(st->last_no, no) == 0 &&
        now_ms - st->last_ms < window_ms)
        return false;                     /* 同卡窗内:忽略(不弹窗不落日志) */
    snprintf(st->last_no, sizeof(st->last_no), "%s", no);
    st->last_ms = now_ms;
    return true;
}

static void *provider_thread(void *arg)
{
    (void)arg;
    int err_streak = 0;

    while (s_running) {
        if (s_fd < 0) {
            s_fd = iccard_hal_open(cfg_get()->iccard_dev_path);
            if (s_fd < 0) {
                degrade();
                sleep(2);                 /* 退避重试(协议 §7.1) */
                continue;
            }
        }

        if (s_flush_req) {
            s_flush_req = false;
            iccard_hal_flush(s_fd);
        }

        int pr = iccard_hal_poll(s_fd, 200);
        if (pr < 0) {
            if (++err_streak >= 5) {      /* 连续 5 次通信错 → 降级 */
                degrade();
                iccard_hal_close(s_fd);
                s_fd = -1;
                err_streak = 0;
                sleep(2);
            }
            continue;
        }
        if (pr == 0)
            continue;

        dg_iccard_frame_t f;
        if (iccard_hal_read(s_fd, &f) != DG_OK) {
            if (++err_streak >= 5) {
                degrade();
                iccard_hal_close(s_fd);
                s_fd = -1;
                err_streak = 0;
                sleep(2);
            }
            continue;
        }
        err_streak = 0;
        recover();

        if (!iccard_frame_valid(&f)) {
            DG_LOGW(TAG, "坏帧丢弃(magic=%08x uid_len=%u)", f.magic, f.uid_len);
            continue;
        }

        char no[DG_IC_LEN];
        if (iccard_uid_to_hex(f.uid, f.uid_len, no) != DG_OK) {
            /* 16B UID 超 DG_IC_LEN 承载力(iccard_hal.c 注),显式拒收不留痕丢失 */
            DG_LOGW(TAG, "UID 长度 %u 超卡号串上限,丢弃", f.uid_len);
            continue;
        }

        /* 防重窗不在这里做:协议 §7.1 明确它是"普通开门分支专用",而
         * provider 不知道 FSM 状态——每帧原样发布,由 access_service 按
         * FSM 状态分流时执行窗检查(v_ic/录入分支天然单发,不需要窗)。
         * 日志掩码:协议 §5 界面与日志一律掩码 */
        char m[13];
        iccard_mask(no, m, sizeof(m));
        DG_LOGI(TAG, "读到卡 %s(type=%u seq=%u)", m, f.card_type, f.seq);

        ev_ic_card_t ev;
        memset(&ev, 0, sizeof(ev));
        snprintf(ev.card_no, sizeof(ev.card_no), "%s", no);
        EVENT_BUS_PUBLISH(EV_IC_CARD, &ev);
    }
    return NULL;
}

/* 换会话清缓冲:录入/验证切换防旧卡串场(ICCARD_PROTOCOL §4);fd 由
 * provider 线程独占,这里只置请求标志,线程在下一轮 poll 前执行 */
static int on_ctrl(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    const ev_iccard_ctrl_t *c = (const ev_iccard_ctrl_t *)e->data;
    if (c->op == DG_ICCARD_CTRL_FLUSH)
        s_flush_req = true;
    return 0;
}

int card_provider_start(void)
{
    if (s_running)
        return DG_OK;
    s_running = true;
    s_sub = event_bus_subscribe(EV_ICCARD_CTRL, on_ctrl, NULL);
    if (pthread_create(&s_tid, NULL, provider_thread, NULL) == 0) {
        s_tid_created = true;
        DG_LOGI(TAG, "card_provider 启动(dev=%s)", cfg_get()->iccard_dev_path);
        return DG_OK;
    }
    s_running = false;
    event_bus_unsubscribe(s_sub);
    s_sub = NULL;
    DG_LOGE(TAG, "provider 线程创建失败");
    return DG_ERR_INTERNAL;
}

void card_provider_stop(void)
{
    if (!s_running)
        return;
    s_running = false;
    if (s_tid_created) {
        pthread_join(s_tid, NULL);        /* poll 200ms 粒度自醒,秒级退出 */
        s_tid_created = false;
    }
    if (s_fd >= 0) {
        iccard_hal_close(s_fd);
        s_fd = -1;
    }
    event_bus_unsubscribe(s_sub);
    s_sub = NULL;
    s_ready = false;
}
