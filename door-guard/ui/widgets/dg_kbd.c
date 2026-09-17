/*
 * dg_kbd.c — 数字键盘实现
 */
#include "dg_kbd.h"
#include "dg_btn.h"
#include "../theme.h"
#include "dg_log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    dg_kbd_ops_t ops;
} dg_kbd_t;

static void emit_digit(lv_event_t *e)
{
    dg_kbd_t *k = lv_event_get_user_data(e);
    /* 键字符存在按钮 obj user_data(静态字符串),不依赖子树结构 */
    const char *sym = lv_obj_get_user_data(lv_event_get_target(e));
    if (k->ops.on_key)
        k->ops.on_key(k->ops.user_data, sym);
}

static void emit_backspace(lv_event_t *e)
{
    dg_kbd_t *k = lv_event_get_user_data(e);
    if (k->ops.on_backspace)
        k->ops.on_backspace(k->ops.user_data);
}

static void emit_ok(lv_event_t *e)
{
    dg_kbd_t *k = lv_event_get_user_data(e);
    if (k->ops.on_ok)
        k->ops.on_ok(k->ops.user_data);
}

lv_obj_t *dg_kbd_create(lv_obj_t *parent, const dg_kbd_ops_t *ops)
{
    dg_kbd_t *k = malloc(sizeof(dg_kbd_t));
    if (!k) {
        DG_LOGE("[KBD]", "alloc failed");
        return NULL;
    }
    k->ops = *ops;

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_user_data(grid, k);

    static const char *keys[] = { "1", "2", "3", "4", "5", "6",
                                  "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK };
    for (int i = 0; i < 12; i++) {
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, LV_PCT(29), 88);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW_WRAP);

        lv_obj_t *b;
        if (!strcmp(keys[i], LV_SYMBOL_OK)) {
            b = dg_btn_create(cell, keys[i], "");
            lv_obj_set_style_bg_color(b, DG_COL_OK(), 0);
            lv_obj_add_event_cb(b, emit_ok, LV_EVENT_CLICKED, k);
        } else if (!strcmp(keys[i], LV_SYMBOL_BACKSPACE)) {
            b = dg_btn_create_light(cell, keys[i], "");
            lv_obj_add_event_cb(b, emit_backspace, LV_EVENT_CLICKED, k);
        } else {
            b = dg_btn_create(cell, NULL, keys[i]);
            lv_obj_set_user_data(b, (void *)keys[i]);
            lv_obj_add_event_cb(b, emit_digit, LV_EVENT_CLICKED, k);
        }
        lv_obj_set_height(b, 88);
    }
    return grid;
}
