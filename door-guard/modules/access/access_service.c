/*
 * access_service.c — 认证融合服务实现
 *
 * 事件流:
 *   vision(EV_VISION_MATCH_1N) ─┐
 *   UI(EV_UI_BTN/TEXT/METHOD)  ─┼─▶ auth_fsm ──动作──▶ 本服务执行:
 *   tasker 心跳(FSM_EV_TICK)  ─┘      WRITE_LOG→db_log_append+EV_AUTH_RESULT
 *                                      OPEN_DOOR→EV_AUTH_DOOR_OPEN(Phase 8 接 gpio)
 *                                      GOTO_PAGE/HINT→EV_UI_*  (UI 渲染)
 *   UID 提交:本服务 db_user_get 解析(spec: FSM 不碰 DB)
 *   密码提交:db_verify_password + 连错锁定预检
 */
#include "access_service.h"
#include "auth_fsm.h"
#include "hal/gpio/gpio_hal.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "tasker.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 定长拷贝(截断为显式语义) */
static void copy_cstr(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *TAG = "[ACCESS]";

static auth_fsm_t s_fsm;
static event_subscription_t *s_subs[8];
static int s_sub_cnt = 0;
static struct task_node s_tick_node;
static bool s_running = false;

/* ---- 动作执行 ---- */

static void pub_auth_result(const access_log_t *l)
{
    ev_auth_result_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.has_user = l->user_id[0] != '\0';
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", l->user_id);
    snprintf(ev.user_name, sizeof(ev.user_name), "%s", l->user_name);
    ev.method = l->method;
    ev.result = l->result;
    ev.reason = l->reason;
    ev.ts = l->ts;
    EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &ev);
}

static void act_log_write(const fsm_log_act_t *l)
{
    access_log_t log;
    memset(&log, 0, sizeof(log));
    log.ts = l->ts ? l->ts : (int64_t)time(NULL);   /* FSM 未带时刻则以落库时刻为准 */
    log.has_user = l->user_id[0] != '\0';
    snprintf(log.user_id, sizeof(log.user_id), "%s", l->user_id);
    snprintf(log.user_name, sizeof(log.user_name), "%s", l->user_name);
    log.method = l->method;
    log.result = l->result;
    log.reason = l->reason;
    if (db_log_append(&log) != DG_OK)
        DG_LOGE(TAG, "日志落库失败");
    pub_auth_result(&log);                          /* web 上位机同源 */
}

/* FSM 结果弹窗延迟到日志落库后(UI 弹窗文案取自 EV_AUTH_RESULT) */
static struct {
    bool pending;
    bool ok;
} s_popup_defer;

typedef struct {
    int32_t timer_id;
    uint32_t seq;
} tmr_user_t;

static void fsm_timer_pub(void *raw)
{
    tmr_user_t *u = raw;
    ev_access_timer_t ev;
    ev.timer_id = u->timer_id;
    ev.seq = u->seq;
    free(u);
    EVENT_BUS_PUBLISH(EV_ACCESS_TIMER, &ev);
}

static enum task_t fsm_timer_pub_task(void *ctx)
{
    fsm_timer_pub(ctx);
    return TASK_OK;
}

static enum task_t fsm_timer_pub_task(void *ctx);

static void arm_fsm_timer(int32_t id, uint32_t ms, uint32_t seq)
{
    tmr_user_t *u = malloc(sizeof(*u));
    if (!u)
        return;
    u->timer_id = id;
    u->seq = seq;
    char name[32];
    snprintf(name, sizeof(name), "fsm_tmr_%u", seq);
    /* period=ms + run_cnt=1:sched 线程按最近截止时间唤醒,精度满足 1.5s 窗口 */
    struct task_node node;
    if (tasker_task_init_li(&node, (int)ms, 1, name, fsm_timer_pub_task, u)
            != TASK_OK ||
        tasker_enqueue(&node) != TASK_OK) {
        free(u);
        DG_LOGE(TAG, "定时器任务入队失败 id=%d", id);
    }
}

static void on_fsm_action(fsm_action_t act, const fsm_action_data_t *d, void *ud)
{
    (void)ud;
    switch (act) {
    case FSM_ACT_GOTO_PAGE: {
        ev_goto_page_t ev;
        snprintf(ev.page, sizeof(ev.page), "%s", d->page);
        EVENT_BUS_PUBLISH(EV_UI_GOTO_PAGE, &ev);
        break;
    }
    case FSM_ACT_POPUP_SUCCESS:
    case FSM_ACT_POPUP_FAIL:
        /* 结果弹窗推迟到日志落库(EV_AUTH_RESULT 先行,UI 取文案) */
        s_popup_defer.pending = true;
        s_popup_defer.ok = (act == FSM_ACT_POPUP_SUCCESS);
        break;
    case FSM_ACT_HINT_TEXT: {
        ev_hint_t ev;
        ev.method = d->misc.method;
        EVENT_BUS_PUBLISH(EV_UI_HINT, &ev);
        break;
    }
    case FSM_ACT_OPEN_DOOR: {
        /* 门控:gpio_hal 脉冲(引脚配置 device.json;失败不阻断结果事件) */
        static bool gpio_ready = false;
        if (!gpio_ready) {
            if (gpio_hal_init(cfg_get()->relay_gpio_line) == DG_OK)
                gpio_ready = true;
            else
                DG_LOGW(TAG, "gpio 初始化失败,门控仅事件可观测");
        }
        if (gpio_ready && gpio_hal_door_pulse(d->door_open_ms) != DG_OK)
            DG_LOGE(TAG, "开门脉冲失败");
        ev_door_state_t ev = { .open = true };
        EVENT_BUS_PUBLISH(EV_AUTH_DOOR_OPEN, &ev);
        DG_LOGI(TAG, "开门 %ums", d->door_open_ms);
        break;
    }
    case FSM_ACT_WRITE_LOG:
        act_log_write(&d->log);
        if (s_popup_defer.pending) {
            ev_hint_t ev;
            ev.method = s_popup_defer.ok ? -3 : -4;
            EVENT_BUS_PUBLISH(EV_UI_HINT, &ev);
            s_popup_defer.pending = false;
        }
        break;
    case FSM_ACT_SET_TIMER:
        arm_fsm_timer(d->timer.timer_id, d->timer.ms, d->timer.seq);
        break;
    case FSM_ACT_CANCEL_TIMERS:
        /* 过期定时器由 timer_seq 丢弃机制兜底,tasker 任务自然到期 */
        break;
    default:
        break;                                      /* FACEBOX 等由 UI 直订视觉事件 */
    }
}

/* ---- 视觉事件 → FSM ---- */

static int on_match(const event_t *e, void *ud)
{
    (void)ud;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.match = *(const ev_match_t *)e->data;
    auth_fsm_handle(&s_fsm, FSM_EV_MATCH_1N, &d);
    return 0;
}

static int on_face_box(const event_t *e, void *ud)
{
    (void)ud;
    const ev_face_box_t *b = (const ev_face_box_t *)e->data;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.box.x = b->x; d.box.y = b->y; d.box.w = b->w; d.box.h = b->h;
    auth_fsm_handle(&s_fsm, FSM_EV_FACE_DETECTED, &d);
    return 0;
}

/* ---- UI 请求 → FSM ---- */

static int on_btn(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ui_btn_t *b = (const ev_ui_btn_t *)e->data;
    fsm_event_t ev = (b->btn == DG_BTN_MENU)    ? FSM_EV_MENU_BTN
                     : (b->btn == DG_BTN_VERIFY) ? FSM_EV_VERIFY_BTN
                                                 : FSM_EV_BACK;
    auth_fsm_handle(&s_fsm, ev, NULL);
    return 0;
}

static int on_text_input(const event_t *e, void *ud)
{
    (void)ud;
    const ev_text_input_t *t = (const ev_text_input_t *)e->data;

    if (t->kind == DG_INPUT_UID) {
        fsm_event_data_t d;
        memset(&d, 0, sizeof(d));
        copy_cstr(d.uid, sizeof(d.uid), t->text);
        auth_fsm_handle(&s_fsm, FSM_EV_UID_SUBMIT, &d);

        /* 服务层解析 ID(FSM 不碰 DB) */
        user_rec_t rec;
        fsm_event_data_t rd;
        memset(&rd, 0, sizeof(rd));
        rd.uid_res.found = false;
        copy_cstr(rd.uid_res.user_id, sizeof(rd.uid_res.user_id), t->text);
        if (db_user_get(t->text, &rec) == DG_OK) {
            rd.uid_res.found = true;
            rd.uid_res.role = rec.role;
            rd.uid_res.auth_flags = rec.auth_flags;
            snprintf(rd.uid_res.user_name, sizeof(rd.uid_res.user_name), "%s",
                     rec.user_name);
        }
        auth_fsm_handle(&s_fsm, FSM_EV_UID_RESOLVED, &rd);
        return 0;
    }

    if (t->kind == DG_INPUT_PWD) {
        fsm_event_data_t d;
        memset(&d, 0, sizeof(d));
        d.result.method = DG_METHOD_PWD;
        d.result.now_ms = (int64_t)time(NULL) * 1000;

        if (auth_fsm_pwd_locked(&s_fsm, t->uid, d.result.now_ms)) {
            d.result.locked = true;                 /* 锁定中直接拒,不计次数 */
            d.result.ok = false;
            d.result.reason = DG_REASON_WRONG_PWD;
        } else {
            user_rec_t rec;
            int rc = db_verify_password(t->uid, t->text, &rec);
            d.result.ok = (rc == DG_OK);
            d.result.reason = (rc == DG_OK) ? DG_REASON_OK
                              : (rc == DG_ERR_WRONG_PASSWORD ? DG_REASON_WRONG_PWD
                                                             : DG_REASON_NO_USER);
            if (d.result.ok) {
                snprintf(d.result.user_id, sizeof(d.result.user_id), "%s", rec.user_id);
                snprintf(d.result.user_name, sizeof(d.result.user_name), "%s",
                         rec.user_name);
            }
        }
        auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_RESULT, &d);
    }
    return 0;
}

static int on_method_pick(const event_t *e, void *ud)
{
    (void)ud;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.method = ((const ev_method_pick_t *)e->data)->method;
    auth_fsm_handle(&s_fsm, FSM_EV_METHOD_PICK, &d);
    return 0;
}

static int on_touch(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    auth_fsm_handle(&s_fsm, FSM_EV_TOUCH, NULL);
    return 0;
}

static int on_access_tick(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    return 0;
}

/* tasker 一次性任务到点 → 经总线回注(保证 FSM 全部在 bus 线程驱动) */
static int on_access_timer(const event_t *e, void *ud)
{
    (void)ud;
    const ev_access_timer_t *t = (const ev_access_timer_t *)e->data;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = (int32_t)t->timer_id;
    d.timer.seq = t->seq;
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
    return 0;
}

/* ---- 1s 心跳(tasker 短任务) ---- */

static enum task_t tick_task(void *ctx)
{
    (void)ctx;
    /* 回注总线:FSM 只在 event_bus 分发线程驱动(与事件处理互斥) */
    if (s_running)
        EVENT_BUS_PUBLISH_EMPTY(EV_ACCESS_TICK);
    return TASK_OK;
}

int access_service_start(void)
{
    if (s_running)
        return DG_OK;
    auth_fsm_init(&s_fsm, cfg_get()->door_open_ms, cfg_get()->standby_timeout_s,
                  cfg_get()->pwd_fail_lock_n, cfg_get()->pwd_fail_lock_s,
                  on_fsm_action, NULL);

    s_sub_cnt = 0;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_MATCH_1N, on_match, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_FACE_BOX, on_face_box, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_UI_BTN, on_btn, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_UI_TEXT_INPUT, on_text_input, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_UI_METHOD_PICK, on_method_pick, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_UI_TOUCH, on_touch, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_ACCESS_TIMER, on_access_timer, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_ACCESS_TICK, on_access_tick, NULL);

    if (tasker_task_init_li(&s_tick_node, 1000, TASK_CNT_INF, "access_tick",
                            tick_task, NULL) != TASK_OK ||
        tasker_enqueue(&s_tick_node) != TASK_OK) {
        DG_LOGE(TAG, "心跳任务入队失败");
        return DG_ERR_INTERNAL;
    }
    s_running = true;
    DG_LOGI(TAG, "access 服务启动");
    return DG_OK;
}

void access_service_stop(void)
{
    tasker_cancel_by_name("access_tick");
    for (int i = 0; i < s_sub_cnt; i++) {
        event_bus_unsubscribe(s_subs[i]);
        s_subs[i] = NULL;
    }
    s_sub_cnt = 0;
    s_running = false;
}
