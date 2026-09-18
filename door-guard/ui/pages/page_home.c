/*
 * page_home.c — 主页视图(spec-ui §3.1):推流画布 + 脸框 + 提示条 + 按钮
 *
 * 分层(pages 层,纯视图):建控件、33ms 刷画布、按钮点击转 bridge 动作;
 * 渲染内容事件经 setter 注入(page_home_set_*),业务在 presenter_home。
 */
#include "page_home.h"
#include "bridge/bridge.h"
#include "dg_log.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include "hal/camera/camera.h"

#include <stdlib.h>
#include <string.h>

static lv_obj_t *s_canvas = NULL;
static lv_color_t *s_canvas_buf = NULL;
static lv_obj_t *s_facebox = NULL;
static lv_obj_t *s_hint = NULL;
static lv_timer_t *s_pump_timer = NULL;

/* ---- 相机帧 → 画布(33ms 轮询,30fps) ---- */

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
    if (f->w == w && f->h == h) {
        lv_canvas_copy_buf(s_canvas, (const lv_color_t *)f->pixels, 0, 0, w, h);
        lv_obj_invalidate(s_canvas);
    }
}

/* ---- 按钮 → bridge 动作 ---- */

static void click_pub(const ev_ui_btn_t *btn)
{
    dg_popup_close();
    bridge_btn(btn);
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

    s_pump_timer = lv_timer_create(canvas_timer_cb, 33, NULL); /* 30fps:与传感器帧率对齐 */
}

void page_home_destroy(void)
{
    DG_LOGI("[HOME]", "page destroy");
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

/* ---- setter(presenter 渲染入口) ---- */

void page_home_set_facebox(int state, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s_facebox)
        return;
    lv_color_t c = (state == DG_BOX_MATCHED)  ? DG_COL_OK()
                   : (state == DG_BOX_FAILED) ? DG_COL_ERR()
                                              : DG_COL_WARN();
    lv_obj_clear_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_facebox, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_size(s_facebox, (lv_coord_t)w, (lv_coord_t)h);
    lv_obj_set_style_border_color(s_facebox, c, 0);
    lv_obj_invalidate(s_facebox);
}

void page_home_clear_facebox(void)
{
    if (s_facebox)
        lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
}

void page_home_set_hint(const char *text)
{
    if (!s_hint)
        return;
    if (!text) {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_hint, text);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}
