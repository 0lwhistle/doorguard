/*
 * page_display_set.c — 屏幕显示设置页(M1/M2 功耗管理,2026-10-04)
 *
 * 背光亮度滑条(10~100):拖动 LVGL 线程直写 display 原语即时预览(毫秒级
 * sysfs,零总线流量);松手 cfg_set("brightness") 持久化 + bridge_brightness
 * 事件回执执行(同值幂等,web 上位机将来走同一条生效通道)。
 * 滑条交互与 page_face_set 同款:VALUE_CHANGED 只刷数字,RELEASED 落盘。
 * 范围与 cfg META 一致(10~100):0 会让用户全黑摸不回,下限 10 防呆。
 */
#include "page_display_set.h"
#include "cfg.h"
#include "modules/display/display.h"
#include "dg_log.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "bridge/bridge.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdio.h>

static lv_obj_t *s_slider = NULL;
static lv_obj_t *s_value = NULL;

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

static void slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const int v = lv_slider_get_value(s_slider);

    if (code == LV_EVENT_VALUE_CHANGED) {
        char t[8];
        snprintf(t, sizeof(t), "%d%%", v);
        if (s_value)
            lv_label_set_text(s_value, t);
        /* 拖动即时预览(直写 sysfs,毫秒级);未落盘 */
        (void)display_backlight_set(v);
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (cfg_set_int("brightness", v) == DG_OK) {
            bridge_brightness(v);       /* 生效统一走事件通道(幂等) */
            dg_popup_success(_("已保存"), 600, NULL, NULL);
        } else {
            dg_popup_fail(_("保存失败"), 1000, NULL, NULL);
        }
    }
}

void page_display_set_create(lv_obj_t *parent)
{
    DG_LOGI("[DISPLAY_SET]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("屏幕显示"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tlabel = lv_label_create(parent);
    lv_label_set_text(tlabel, _("屏幕亮度"));
    lv_obj_set_style_text_font(tlabel, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(tlabel, DG_COL_TEXT(), 0);
    lv_obj_align(tlabel, LV_ALIGN_TOP_LEFT, DG_PAD, 160);

    lv_obj_t *val = lv_label_create(parent);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_OK(), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -DG_PAD, 160);
    s_value = val;

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, DG_SCREEN_W - 2 * DG_PAD, 56);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, DG_PAD, 204);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, cfg_get()->brightness, LV_ANIM_OFF);
    /* 蓝白主题:已填充段用主色,旋钮加大触摸区(与 face_set 同款) */
    lv_obj_set_style_bg_color(slider, DG_COL_SCRIM(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, DG_COL_OK(), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_RELEASED, NULL);
    s_slider = slider;

    char t[8];
    snprintf(t, sizeof(t), "%d%%", cfg_get()->brightness);
    lv_label_set_text(val, t);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, _("拖动即时生效,松手保存"));
    lv_obj_set_style_text_font(hint, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(hint, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, DG_PAD, 276);

    lv_obj_t *hint2 = lv_label_create(parent);
    lv_label_set_text(hint2, _("待机时自动降至 30%,10 秒无操作全黑"));
    lv_obj_set_style_text_font(hint2, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(hint2, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_opa(hint2, LV_OPA_70, 0);
    lv_obj_align(hint2, LV_ALIGN_TOP_LEFT, DG_PAD, 320);
}

void page_display_set_destroy(void)
{
    s_slider = NULL;
    s_value = NULL;
    DG_LOGI("[DISPLAY_SET]", "page destroy");
}
