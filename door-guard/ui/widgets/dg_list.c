/*
 * dg_list.c — 列表实现
 */
#include "dg_list.h"
#include "../theme.h"

lv_obj_t *dg_list_create(lv_obj_t *parent)
{
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, DG_PAD, 0);
    return list;
}

lv_obj_t *dg_list_add_row(lv_obj_t *list, const void *icon, const char *text,
                          void (*on_click)(lv_event_t *e))
{
    lv_obj_t *row = lv_list_add_btn(list, icon, text);
    lv_obj_set_style_text_font(row, DG_FONT_CN, 0);
    lv_obj_set_style_bg_color(row, DG_COL_BG_LIGHT(), 0);
    lv_obj_set_style_radius(row, DG_RADIUS, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    if (on_click)
        lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

void dg_list_clear(lv_obj_t *list)
{
    lv_obj_clean(list);
}
