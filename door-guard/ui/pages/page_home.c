/*
 * page_home.c — 主页(spec-ui §3.1):推流层 + 脸框 overlay + 菜单/验证按钮
 *
 * Phase 7 后本页只做渲染与事件转发:
 *   按钮/触摸 → EV_UI_BTN / EV_UI_TOUCH(请求发服务)
 *   EV_VISION_FACE_BOX/LOST → 脸框渲染(视觉事件直订,纯绘制数据)
 *   EV_AUTH_RESULT → 结果弹窗(文案取自事件字段)
 *   EV_UI_HINT / EV_UI_GOTO_PAGE → 提示条/切页(服务决策)
 * 业务决策在 auth_fsm(access_service 持有)。
 */
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include "hal/camera/camera.h"
#include "ui_events.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_canvas = NULL;
static lv_color_t *s_canvas_buf = NULL;
static lv_obj_t *s_facebox = NULL;
static lv_coord_t s_box_last[4];
static lv_obj_t *s_hint = NULL;
static lv_timer_t *s_pump_timer = NULL;
static event_subscription_t *s_subs[6];
static int s_sub_cnt = 0;
static ev_auth_result_t s_last_result;       /* 最近认证结果(弹窗文案源) */

static void render_evt(const ui_evt_t *evt);

/* ---- 相机帧 → 画布(33ms 轮询,30fps) ---- */

static void canvas_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 事件泵已上移 ui.c(页面无关);本定时器只负责 canvas 刷帧 */
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
    if (f->w == w && f->h == h) {
        lv_canvas_copy_buf(s_canvas, (const lv_color_t *)f->pixels, 0, 0, w, h);
        lv_obj_invalidate(s_canvas);
    }
}

/* ---- 视觉脸框(纯绘制) ---- */

/* 总线回调(LVGL 禁区):一律入队,LVGL 线程泵出渲染 */

static int on_face_box(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_FACE_BOX;
    evt.box = *(const ev_face_box_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_face_lost(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_FACE_LOST;
    ui_evt_push(&evt);
    return 0;
}

static int on_auth_result(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_AUTH_RESULT;
    evt.result = *(const ev_auth_result_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_hint(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_HINT;
    evt.hint = *(const ev_hint_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_refresh_evt(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    /* 语言切换页重建由刷新事件在 LVGL 定时器内执行更简单:走队列 */
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_HINT;
    evt.hint.method = -5;                   /* -5 = 刷新页 */
    ui_evt_push(&evt);
    return 0;
}

/* ---- LVGL 线程:队列泵出渲染 ---- */

static void render_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_FACE_BOX: {
        const ev_face_box_t *b = &evt->box;
        if (!s_facebox)
            break;
        s_box_last[0] = (lv_coord_t)b->x;
        s_box_last[1] = (lv_coord_t)b->y;
        s_box_last[2] = (lv_coord_t)b->w;
        s_box_last[3] = (lv_coord_t)b->h;
        lv_color_t c = (b->state == DG_BOX_MATCHED)  ? DG_COL_OK()
                       : (b->state == DG_BOX_FAILED) ? DG_COL_ERR()
                                                     : DG_COL_WARN();
        lv_obj_clear_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_facebox, s_box_last[0], s_box_last[1]);
        lv_obj_set_size(s_facebox, s_box_last[2], s_box_last[3]);
        lv_obj_set_style_border_color(s_facebox, c, 0);
        lv_obj_invalidate(s_facebox);
        break;
    }
    case UI_EVT_FACE_LOST:
        if (s_facebox)
            lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
        break;
    case UI_EVT_AUTH_RESULT:
        s_last_result = evt->result;
        break;
    case UI_EVT_HINT: {
        const ev_hint_t *h = &evt->hint;
        if (h->method == -5) {
            navigator_reload();              /* 整页重建(语言刷新) */
            break;
        }
        if (h->method == -3) {
            char text[DG_UID_LEN + DG_NAME_LEN + 8];
            snprintf(text, sizeof(text), "%s %s", _("验证成功"),
                     s_last_result.has_user ? s_last_result.user_name : "");
            dg_popup_success(text, 3000, NULL, NULL);
            break;
        }
        if (h->method == -4) {
            dg_popup_fail(_("验证失败"), 3000, NULL, NULL);
            break;
        }
        if (!s_hint)
            break;
        const char *text = NULL;
        if (h->method == -1)
            text = _("管理员认证");
        else if (h->method == DG_METHOD_FACE_11)
            text = _("请正对摄像头");
        else if (h->method == DG_METHOD_FINGER)
            text = _("请按指纹");
        else if (h->method == DG_METHOD_PWD)
            text = _("请输入密码");
        else if (h->method == DG_METHOD_IC)
            text = _("请刷卡");
        if (text) {
            lv_label_set_text(s_hint, text);
            lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    }
    case UI_EVT_GOTO_PAGE:
        break;                  /* 切页由 ui 层泵直驱 navigator_switch */
    }
}

/* ---- UI 输入 → 服务请求 ---- */

static void click_pub(const ev_ui_btn_t *btn)
{
    dg_popup_close();
    EVENT_BUS_PUBLISH(EV_UI_BTN, btn);
}

static void on_menu_btn(lv_event_t *e)
{
    (void)e;
    ev_ui_btn_t b = { .btn = DG_BTN_MENU };
    click_pub(&b);
}

static void on_verify_btn(lv_event_t *e)
{
    (void)e;
    ev_ui_btn_t b = { .btn = DG_BTN_VERIFY };
    click_pub(&b);
}

/* ---- 生命周期 ---- */

void page_home_create(lv_obj_t *parent)
{
    DG_LOGI("[HOME]", "page create");
    memset(&s_last_result, 0, sizeof(s_last_result));

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
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_UI_HINT, on_hint, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EVENT_UI_REFRESH_REQUEST, on_refresh_evt, NULL);

    s_pump_timer = lv_timer_create(canvas_timer_cb, 33, NULL); /* 30fps:与传感器帧率对齐 */
}

void page_home_destroy(void)
{
    DG_LOGI("[HOME]", "page destroy");
    for (int i = 0; i < s_sub_cnt; i++) {
        event_bus_unsubscribe(s_subs[i]);
        s_subs[i] = NULL;
    }
    s_sub_cnt = 0;
    if (s_pump_timer) {
        lv_timer_del(s_pump_timer);
        s_pump_timer = NULL;
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
    static const navigator_page_t ops = {
        .name = "home",
        .create = page_home_create,
        .destroy = page_home_destroy,
        .on_evt = render_evt,
    };
    navigator_register(&ops);
}
