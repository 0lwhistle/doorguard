/*
 * page_home.c — 主页(spec-ui §3.1):推流层 + 脸框 overlay + 菜单/验证按钮
 *
 * 验证状态机(auth_fsm)在本页驱动:订阅视觉事件喂 FSM,FSM 动作翻译成
 * LVGL 渲染/弹窗/定时器/日志/开门事件。页面不做业务决策(决策在 FSM)。
 */
#include "auth_fsm.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "i18n.h"
#include "page_mgr.h"
#include "storage.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include "hal/camera/camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static auth_fsm_t s_fsm;
static lv_obj_t *s_canvas = NULL;        /* 推流层 */
static lv_color_t *s_canvas_buf = NULL;  /* 帧缓冲(帧尺寸就绪时分配) */
static lv_obj_t *s_facebox = NULL;       /* 脸框 overlay */
static lv_coord_t s_box_last[4];         /* 最近框位置(w=0 重绘沿用) */
static lv_obj_t *s_hint = NULL;          /* 提示条(管理员认证/请正对摄像头) */
static lv_timer_t *s_fsm_timers[FSM_TMR_COUNT];
static lv_timer_t *s_pump_timer = NULL;
static lv_timer_t *s_tick_timer = NULL;
static event_subscription_t *s_subs[4];
static int s_sub_cnt = 0;

static void on_fsm_action(fsm_action_t act, const fsm_action_data_t *d, void *ud);

/* ---- 相机帧 → 画布(100ms 轮询) ---- */

static void canvas_timer_cb(lv_timer_t *t)
{
    (void)t;
    const camera_frame_t *f = camera_latest();
    if (!f || !s_canvas)
        return;
    int32_t w = f->w > DG_SCREEN_W ? DG_SCREEN_W : f->w;
    int32_t h = f->h > DG_SCREEN_H ? DG_SCREEN_H : f->h;
    if (!s_canvas_buf) {
        s_canvas_buf = malloc((size_t)w * h * sizeof(lv_color_t));
        if (!s_canvas_buf)
            return;
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, w, h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
    }
    /* 帧尺寸与画布一致才拷贝(sim 图片统一 720×960 内) */
    if (f->w == w && f->h == h) {
        lv_canvas_copy_buf(s_canvas, (const lv_color_t *)f->pixels, 0, 0, w, h);
        lv_obj_invalidate(s_canvas);
    }
}

/* ---- 待机心跳(TICK 喂 FSM) ---- */

static void tick_timer_cb(lv_timer_t *t)
{
    (void)t;
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
}

/* ---- FSM 定时器桥(SET_TIMER 动作 → 一次性 lv_timer,seq 回填) ---- */

typedef struct {
    fsm_timer_t id;
    uint32_t seq;
} fsm_timer_user_t;

static void fsm_timer_cb(lv_timer_t *t)
{
    fsm_timer_user_t *u = (t->user_data);
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = u->id;
    d.timer.seq = u->seq;
    s_fsm_timers[u->id] = NULL;
    lv_timer_del(t);                    /* 一次性:先摘登记再触发 FSM */
    free(u);
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
}

/* ---- 事件 → FSM 翻译 ---- */

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

static int on_face_lost(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    auth_fsm_handle(&s_fsm, FSM_EV_FACE_LOST, NULL);
    return 0;
}

static int on_match(const event_t *e, void *ud)
{
    (void)ud;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.match = *(const ev_match_t *)e->data;
    auth_fsm_handle(&s_fsm, FSM_EV_MATCH_1N, &d);
    return 0;
}

/* 语言切换后整页重建 */
static int on_refresh_evt(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    page_mgr_open(page_mgr_current());
    return 0;
}

/* ---- UID 解析(页面代跑 storage;Phase 7 收进 access 服务) ---- */

static void on_uid_confirm(void *ud, const char *text)
{
    (void)ud;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.uid, sizeof(d.uid), "%s", text);
    auth_fsm_handle(&s_fsm, FSM_EV_UID_SUBMIT, &d);

    user_rec_t rec;
    fsm_event_data_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.uid_res.found = false;
    if (db_user_get(d.uid, &rec) == DG_OK) {
        rd.uid_res.found = true;
        rd.uid_res.role = rec.role;
        rd.uid_res.auth_flags = rec.auth_flags;
        snprintf(rd.uid_res.user_id, sizeof(rd.uid_res.user_id), "%s", rec.user_id);
        snprintf(rd.uid_res.user_name, sizeof(rd.uid_res.user_name), "%s", rec.user_name);
    }
    auth_fsm_handle(&s_fsm, FSM_EV_UID_RESOLVED, &rd);
}

static void on_uid_cancel(void *ud)
{
    (void)ud;
    /* 取消 = 立即注入当前 5s 步定时器到期(同径回普通) */
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = FSM_TMR_STEP_5S;
    d.timer.seq = s_fsm.timer_active[FSM_TMR_STEP_5S];
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
}

/* ---- 方式选择 → FSM ---- */

static void method_pick_cb(void *ud, int idx)
{
    (void)ud;
    static const int32_t map[] = { DG_METHOD_FACE_11, DG_METHOD_FINGER,
                                   DG_METHOD_PWD, DG_METHOD_IC };
    if (idx < 0 || idx >= 4)
        return;
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.method = map[idx];
    auth_fsm_handle(&s_fsm, FSM_EV_METHOD_PICK, &d);
}

/* ---- FSM 动作 → UI/服务执行 ---- */

static void act_facebox(const fsm_facebox_act_t *fb)
{
    if (!s_facebox)
        return;
    if (fb->box.w > 0) {                /* w=0 = 沿用现有位置 */
        s_box_last[0] = (lv_coord_t)fb->box.x;
        s_box_last[1] = (lv_coord_t)fb->box.y;
        s_box_last[2] = (lv_coord_t)fb->box.w;
        s_box_last[3] = (lv_coord_t)fb->box.h;
    }
    lv_obj_clear_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_facebox, s_box_last[0], s_box_last[1]);
    lv_obj_set_size(s_facebox, s_box_last[2], s_box_last[3]);
    lv_color_t c;
    switch (fb->state) {
    case DG_BOX_MATCHED:  c = DG_COL_OK();   break;
    case DG_BOX_FAILED:   c = DG_COL_ERR();  break;
    default:              c = DG_COL_WARN(); break;
    }
    lv_obj_set_style_border_color(s_facebox, c, 0);
    lv_obj_invalidate(s_facebox);
}

static void act_log_write(const fsm_log_act_t *l)
{
    access_log_t log;
    memset(&log, 0, sizeof(log));
    log.ts = l->ts ? l->ts : (int64_t)time(NULL);
    log.has_user = l->user_id[0] != '\0';
    snprintf(log.user_id, sizeof(log.user_id), "%s", l->user_id);
    snprintf(log.user_name, sizeof(log.user_name), "%s", l->user_name);
    log.method = l->method;
    log.result = l->result;
    log.reason = l->reason;
    if (db_log_append(&log) != DG_OK)
        DG_LOGE("[HOME]", "日志落库失败");

    /* 认证结果广播(web 上位机订阅;Phase 7 服务层接管唯一出口) */
    ev_auth_result_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.has_user = log.has_user;
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", log.user_id);
    snprintf(ev.user_name, sizeof(ev.user_name), "%s", log.user_name);
    ev.method = l->method;
    ev.result = l->result;
    ev.reason = l->reason;
    ev.ts = log.ts;
    EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &ev);
}

static void act_popup_result(bool ok, const fsm_misc_act_t *m)
{
    /* misc.hint = "user_id|user_name" */
    char name[DG_NAME_LEN] = "";
    const char *sep = m->hint[0] ? strchr(m->hint, '|') : NULL;
    if (sep)
        snprintf(name, sizeof(name), "%s", sep + 1);
    char text[DG_UID_LEN + DG_NAME_LEN + 8];
    if (ok)
        snprintf(text, sizeof(text), "%s %s", _("验证成功"), name);
    else
        snprintf(text, sizeof(text), "%s", _("验证失败"));
    if (ok)
        dg_popup_success(text, 3000, NULL, NULL);
    else
        dg_popup_fail(text, 3000, NULL, NULL);
}

/* 方式选择弹窗(按 auth_flags 列出;密码必开兜底,spec §4.2) */
static void act_show_methods(uint32_t auth_flags)
{
    /* 选项顺序与 method_pick_cb 的 map 一致:face11/finger/pwd/ic */
    static const char *opts[4];
    int cnt = 0;
    if (auth_flags & DG_AUTH_FACE)
        opts[cnt++] = _("1:1人脸");
    if (auth_flags & DG_AUTH_FINGER)
        opts[cnt++] = _("指纹");
    if (auth_flags & DG_AUTH_PWD)
        opts[cnt++] = _("密码");
    if (auth_flags & DG_AUTH_IC)
        opts[cnt++] = _("IC卡");
    if (cnt == 0)
        opts[cnt++] = _("密码");        /* 密码必开兜底(spec-database §1) */
    dg_popup_choice(_("验证方式"), opts, cnt, method_pick_cb, NULL, NULL);
}

static void act_hint(const fsm_misc_act_t *m)
{
    if (!s_hint)
        return;
    const char *text = NULL;
    if (m->method == -1)
        text = _("管理员认证");
    else if (m->method == DG_METHOD_FACE_11)
        text = _("请正对摄像头");
    else if (m->method == DG_METHOD_FINGER)
        text = _("请按指纹");
    else if (m->method == DG_METHOD_PWD)
        text = _("请输入密码");
    else if (m->method == DG_METHOD_IC)
        text = _("请刷卡");
    if (text) {
        lv_label_set_text(s_hint, text);
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_fsm_action(fsm_action_t act, const fsm_action_data_t *d, void *ud)
{
    (void)ud;
    switch (act) {
    case FSM_ACT_GOTO_PAGE:
        page_mgr_open(d->page);         /* standby/home/menu */
        break;
    case FSM_ACT_FACEBOX:
        act_facebox(&d->facebox);
        break;
    case FSM_ACT_FACEBOX_HIDE:
        if (s_facebox)
            lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
        break;
    case FSM_ACT_POPUP_SUCCESS:
        act_popup_result(true, &d->misc);
        break;
    case FSM_ACT_POPUP_FAIL:
        act_popup_result(false, &d->misc);
        break;
    case FSM_ACT_ASK_UID:
        dg_popup_input(_("请输入用户ID"), false, on_uid_confirm, on_uid_cancel, NULL);
        break;
    case FSM_ACT_SHOW_METHODS:
        act_show_methods(d->misc.auth_flags);
        break;
    case FSM_ACT_SET_TIMER: {
        fsm_timer_user_t *u = malloc(sizeof(*u));
        if (!u)
            break;
        u->id = (fsm_timer_t)d->timer.timer_id;
        u->seq = d->timer.seq;
        s_fsm_timers[u->id] = lv_timer_create(fsm_timer_cb, d->timer.ms, u);
        lv_timer_set_repeat_count(s_fsm_timers[u->id], 1);
        break;
    }
    case FSM_ACT_CANCEL_TIMERS:
        for (int i = 0; i < FSM_TMR_COUNT; i++) {
            if (s_fsm_timers[i]) {
                lv_timer_del(s_fsm_timers[i]);
                s_fsm_timers[i] = NULL;
            }
        }
        break;
    case FSM_ACT_OPEN_DOOR: {
        /* 门控执行在 Phase 7/8;当前广播事件(可观测) */
        ev_door_state_t ev = { .open = true };
        EVENT_BUS_PUBLISH(EV_AUTH_DOOR_OPEN, &ev);
        DG_LOGI("[HOME]", "开门 %ums", d->door_open_ms);
        break;
    }
    case FSM_ACT_WRITE_LOG:
        act_log_write(&d->log);
        break;
    case FSM_ACT_HINT_TEXT:
        act_hint(&d->misc);
        break;
    case FSM_ACT_HINT_CLEAR:
        if (s_hint)
            lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

/* ---- 按钮事件 ---- */

static void on_menu_btn(lv_event_t *e)
{
    (void)e;
    dg_popup_close();
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
}

static void on_verify_btn(lv_event_t *e)
{
    (void)e;
    dg_popup_close();
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
}

/* ---- 生命周期 ---- */

void page_home_create(lv_obj_t *parent)
{
    DG_LOGI("[HOME]", "page create");
    memset(s_fsm_timers, 0, sizeof(s_fsm_timers));
    auth_fsm_init(&s_fsm, cfg_get()->door_open_ms, cfg_get()->standby_timeout_s,
                  cfg_get()->pwd_fail_lock_n, cfg_get()->pwd_fail_lock_s,
                  on_fsm_action, NULL);

    s_canvas = lv_canvas_create(parent);

    s_facebox = lv_obj_create(parent);
    lv_obj_remove_style_all(s_facebox);
    lv_obj_set_style_border_width(s_facebox, 4, 0);
    lv_obj_set_style_border_color(s_facebox, DG_COL_WARN(), 0);
    lv_obj_set_style_radius(s_facebox, 8, 0);
    lv_obj_set_style_bg_opa(s_facebox, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_hint, DG_COL_TEXT(), 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *menu_btn = dg_btn_create(parent, LV_SYMBOL_SETTINGS, _("菜单"));
    lv_obj_set_size(menu_btn, 200, DG_BTN_H);
    lv_obj_align(menu_btn, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(menu_btn, on_menu_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *verify_btn = dg_btn_create(parent, LV_SYMBOL_OK, _("验证"));
    lv_obj_set_size(verify_btn, 200, DG_BTN_H);
    lv_obj_align(verify_btn, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(verify_btn, on_verify_btn, LV_EVENT_CLICKED, NULL);

    s_sub_cnt = 0;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_FACE_BOX, on_face_box, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_FACE_LOST, on_face_lost, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_MATCH_1N, on_match, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EVENT_UI_REFRESH_REQUEST, on_refresh_evt, NULL);

    s_pump_timer = lv_timer_create(canvas_timer_cb, 100, NULL);
    s_tick_timer = lv_timer_create(tick_timer_cb, 1000, NULL);  /* 待机心跳 */
}

void page_home_destroy(void)
{
    DG_LOGI("[HOME]", "page destroy");
    for (int i = 0; i < s_sub_cnt; i++) {
        event_bus_unsubscribe(s_subs[i]);
        s_subs[i] = NULL;
    }
    s_sub_cnt = 0;
    for (int i = 0; i < FSM_TMR_COUNT; i++) {
        if (s_fsm_timers[i]) {
            lv_timer_del(s_fsm_timers[i]);
            s_fsm_timers[i] = NULL;
        }
    }
    if (s_pump_timer) {
        lv_timer_del(s_pump_timer);
        s_pump_timer = NULL;
    }
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
    if (s_canvas_buf) {
        free(s_canvas_buf);
        s_canvas_buf = NULL;
    }
    s_canvas = NULL;
    s_facebox = NULL;
    s_hint = NULL;
}

void page_home_register(void)
{
    static const dg_page_ops_t ops = {
        .name = "home",
        .create = page_home_create,
        .destroy = page_home_destroy,
    };
    page_mgr_register(&ops);
}

/* 其它页面(待机/菜单)向 FSM 注入事件的入口;FSM 实例为主页静态成员,
 * 跨页存活(返回主页时经 GOTO_PAGE 重建并复位为普通态,语义一致) */
auth_fsm_t *page_home_fsm(void)
{
    return &s_fsm;
}
