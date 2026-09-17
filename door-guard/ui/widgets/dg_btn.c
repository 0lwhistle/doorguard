/*
 * dg_btn.c — 统一按钮实现
 */
#include "dg_btn.h"
#include "theme.h"

static void style_init(lv_obj_t *btn, lv_color_t bg, lv_color_t bg_pressed)
{
    lv_obj_set_size(btn, LV_PCT(100), DG_BTN_H);
    lv_obj_set_style_radius(btn, DG_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_color(btn, bg_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
}

lv_obj_t *dg_btn_create(lv_obj_t *parent, const char *icon, const char *label)
{
    lv_obj_t *btn = lv_btn_create(parent);
    style_init(btn, DG_COLOR_PRIM(), DG_COLOR_DARKC());

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);

    if (icon && *icon) {
        lv_obj_t *sym = lv_label_create(row);
        lv_label_set_text(sym, icon);
        lv_obj_set_style_text_color(sym, DG_COL_BG(), 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);  /* 符号字形在 montserrat 内嵌 */
    }
    lv_obj_t *txt = lv_label_create(row);
    lv_label_set_text(txt, label);
    lv_obj_set_style_text_color(txt, DG_COL_BG(), 0);
    lv_obj_set_style_text_font(txt, DG_FONT_CN, 0);
    return btn;
}

lv_obj_t *dg_btn_create_light(lv_obj_t *parent, const char *icon, const char *label)
{
    lv_obj_t *btn = lv_btn_create(parent);
    style_init(btn, DG_COL_BG_LIGHT(), DG_COLOR_PRIM());

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);

    if (icon && *icon) {
        lv_obj_t *sym = lv_label_create(row);
        lv_label_set_text(sym, icon);
        lv_obj_set_style_text_color(sym, DG_COL_TEXT(), 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);
    }
    lv_obj_t *txt = lv_label_create(row);
    lv_label_set_text(txt, label);
    lv_obj_set_style_text_color(txt, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_font(txt, DG_FONT_CN, 0);
    return btn;
}
