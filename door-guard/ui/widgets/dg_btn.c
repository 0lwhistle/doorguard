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
    /* 半透明白描边:实心控件顶部加一道高光边,轻推层次感(不引入阴影,
     * 全屏重绘架构下阴影是渲染税) */
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, DG_COL_BG(), 0);
    lv_obj_set_style_border_opa(btn, DG_OPA_CARD_LINE, 0);
}

lv_obj_t *dg_btn_create(lv_obj_t *parent, const char *icon, const char *label)
{
    lv_obj_t *btn = lv_button_create(parent);
    style_init(btn, DG_COLOR_PRIM(), DG_COLOR_DARKC());

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    /* v9 行为差异(2026-09-23 板端实测):lv_obj 默认 CLICKABLE,内容行会
     * 命中测试胜出并吞掉 CLICKED(行内 label 已被 v9 置为不可点击,但行
     * 本身不是),点在按钮正中时按钮回调收不到事件——行必须不可点击 */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_t *btn = lv_button_create(parent);
    style_init(btn, DG_COL_BG_LIGHT(), DG_COLOR_PRIM());

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    /* v9 行为差异(2026-09-23 板端实测):lv_obj 默认 CLICKABLE,内容行会
     * 命中测试胜出并吞掉 CLICKED(行内 label 已被 v9 置为不可点击,但行
     * 本身不是),点在按钮正中时按钮回调收不到事件——行必须不可点击 */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
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

void dg_btn_set_label(lv_obj_t *btn, const char *label)
{
    if (!btn || !label)
        return;
    /* 结构约定(见 dg_btn_create):btn → 行容器 → [图标 label?] + 文本 label,
     * 文本 label 必是行容器的最后一个子对象 */
    lv_obj_t *row = lv_obj_get_child(btn, 0);
    uint32_t n = row ? lv_obj_get_child_cnt(row) : 0;
    if (n == 0)
        return;
    lv_obj_t *txt = lv_obj_get_child(row, n - 1);
    if (txt && lv_obj_check_type(txt, &lv_label_class))
        lv_label_set_text(txt, label);
}
