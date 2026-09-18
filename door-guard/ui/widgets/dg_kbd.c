/*
 * dg_kbd.c — 屏幕内键盘实现(数字页 + 字母页)
 *
 * 两页建好后靠 LV_OBJ_FLAG_HIDDEN 切换,**不做对象增删**——切页按钮的点击
 * 事件就在这棵子树里,回调里删自己的祖先会踩 LVGL「事件中途销毁对象」的坑
 * (同类崩溃在待机覆盖层上实测过),隐藏/显示没有这个风险。
 * 字母页大小写:26 个字母键的显示文字由 dg_btn_set_label 改写,**键值不变**
 * (键值是小写串,存在按键 user_data),⇧ 的语义在 emit_key 里落到提交字符上。
 */
#include "dg_kbd.h"
#include "dg_btn.h"
#include "../theme.h"
#include "dg_log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    dg_kbd_ops_t ops;
    lv_obj_t *num_page;
    lv_obj_t *alpha_page;
    lv_obj_t *toggle;             /* 页脚 ABC/123 */
    bool alpha;                   /* 当前是否字母页 */
    bool caps;                    /* 字母页大小写 */
} dg_kbd_t;

/* ---- 键 → 回调 ---- */

static void emit_key(lv_event_t *e)
{
    dg_kbd_t *k = lv_event_get_user_data(e);
    const char *sym = lv_obj_get_user_data(lv_event_get_target(e));
    if (!sym || !k->ops.on_key)
        return;
    if (k->alpha && k->caps && sym[0] >= 'a' && sym[0] <= 'z') {
        char up[2] = { (char)(sym[0] - 'a' + 'A'), '\0' };   /* ⇧ 生效 */
        k->ops.on_key(k->ops.user_data, up);
    } else {
        k->ops.on_key(k->ops.user_data, sym);
    }
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

/* ⇧:改写 26 个字母键的显示文字(键值不动) */
static void refresh_case(dg_kbd_t *k)
{
    uint32_t n = lv_obj_get_child_cnt(k->alpha_page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(k->alpha_page, i);
        uint32_t m = lv_obj_get_child_cnt(row);
        for (uint32_t j = 0; j < m; j++) {
            lv_obj_t *b = lv_obj_get_child(row, j);
            const char *sym = lv_obj_get_user_data(b);
            if (!sym || sym[1] != '\0' || sym[0] < 'a' || sym[0] > 'z')
                continue;                       /* 非字母键(⌫/空格/OK) */
            char up[2] = { (char)(sym[0] - 'a' + 'A'), '\0' };
            dg_btn_set_label(b, k->caps ? up : sym);
        }
    }
}

static void on_shift(lv_event_t *e)
{
    dg_kbd_t *k = lv_event_get_user_data(e);
    k->caps = !k->caps;
    refresh_case(k);
}

/* ---- 页切换(隐藏/显示,不删对象) ---- */

static void apply_layout(dg_kbd_t *k)
{
    if (k->alpha) {
        lv_obj_add_flag(k->num_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(k->alpha_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(k->num_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(k->alpha_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (k->toggle)
        dg_btn_set_label(k->toggle, k->alpha ? "123" : "ABC");
}

void dg_kbd_toggle_layout(lv_obj_t *kbd)
{
    dg_kbd_t *k = kbd ? lv_obj_get_user_data(kbd) : NULL;
    if (!k)
        return;
    k->alpha = !k->alpha;
    k->caps = false;                 /* 换页复位大小写,避免「以为还在大写」 */
    refresh_case(k);
    apply_layout(k);
}

bool dg_kbd_is_alpha(lv_obj_t *kbd)
{
    dg_kbd_t *k = kbd ? lv_obj_get_user_data(kbd) : NULL;
    return k ? k->alpha : false;
}

static void on_toggle(lv_event_t *e)
{
    dg_kbd_t *k = lv_event_get_user_data(e);
    dg_kbd_toggle_layout(lv_obj_get_user_data(k->num_page));
}

/* ---- 键构造 ---- */

static lv_obj_t *add_key(lv_obj_t *parent, dg_kbd_t *k, const char *label,
                         int32_t w_pct, int32_t h, bool light, lv_event_cb_t cb)
{
    lv_obj_t *b = light ? dg_btn_create_light(parent, NULL, label)
                        : dg_btn_create(parent, NULL, label);
    lv_obj_set_width(b, LV_PCT(w_pct));
    lv_obj_set_height(b, h);
    lv_obj_set_user_data(b, (void *)label);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, k);
    return b;
}

/* 字母键:键值 = 小写单字符(堆上,键盘随弹窗销毁时不回收:一次性小对象,
 * 生命周期与页面同;要严格回收可在键盘根对象上挂 LV_EVENT_DELETE) */
static lv_obj_t *add_letter_key(lv_obj_t *parent, dg_kbd_t *k, char lc)
{
    char *sym = malloc(2);
    if (!sym)
        return NULL;
    sym[0] = lc;
    sym[1] = '\0';
    lv_obj_t *b = dg_btn_create_light(parent, NULL, sym);
    lv_obj_set_width(b, LV_PCT(10));
    lv_obj_set_height(b, 62);
    lv_obj_set_user_data(b, sym);
    lv_obj_add_event_cb(b, emit_key, LV_EVENT_CLICKED, k);
    return b;
}

static lv_obj_t *add_row(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *dg_kbd_create(lv_obj_t *parent, bool start_alpha, const dg_kbd_ops_t *ops)
{
    dg_kbd_t *k = calloc(1, sizeof(*k));
    if (!k) {
        DG_LOGE("[KBD]", "alloc failed");
        return NULL;
    }
    k->ops = *ops;
    k->alpha = start_alpha;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_width(root, LV_PCT(100));
    lv_obj_set_height(root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(root, k);

    const int32_t KEY_H = 62;

    /* ---- 数字页:3 列 × 4 行(1-9 / ⌫ 0 OK) ---- */
    k->num_page = lv_obj_create(root);
    lv_obj_remove_style_all(k->num_page);
    lv_obj_set_width(k->num_page, LV_PCT(100));
    lv_obj_set_height(k->num_page, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(k->num_page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(k->num_page, 6, 0);
    lv_obj_set_style_pad_row(k->num_page, 6, 0);
    lv_obj_clear_flag(k->num_page, LV_OBJ_FLAG_SCROLLABLE);

    static const char *const digits[] = { "1", "2", "3", "4", "5", "6",
                                          "7", "8", "9" };
    for (int i = 0; i < 9; i++)
        add_key(k->num_page, k, digits[i], 32, KEY_H, false, emit_key);
    add_key(k->num_page, k, LV_SYMBOL_BACKSPACE, 32, KEY_H, true, emit_backspace);
    add_key(k->num_page, k, "0", 32, KEY_H, false, emit_key);
    {
        lv_obj_t *ok = add_key(k->num_page, k, LV_SYMBOL_OK, 32, KEY_H, false, emit_ok);
        lv_obj_set_style_bg_color(ok, DG_COL_OK(), 0);
    }

    /* ---- 字母页:q..p / a..l ⌫ / ⇧ z..m / 空格 OK ---- */
    k->alpha_page = lv_obj_create(root);
    lv_obj_remove_style_all(k->alpha_page);
    lv_obj_set_width(k->alpha_page, LV_PCT(100));
    lv_obj_set_height(k->alpha_page, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(k->alpha_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(k->alpha_page, 6, 0);
    lv_obj_clear_flag(k->alpha_page, LV_OBJ_FLAG_SCROLLABLE);

    static const char *const rows_abc[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    lv_obj_t *r1 = add_row(k->alpha_page, KEY_H);
    for (int i = 0; rows_abc[0][i]; i++)
        add_letter_key(r1, k, rows_abc[0][i]);

    lv_obj_t *r2 = add_row(k->alpha_page, KEY_H);
    for (int i = 0; rows_abc[1][i]; i++)
        add_letter_key(r2, k, rows_abc[1][i]);
    add_key(r2, k, LV_SYMBOL_BACKSPACE, 10, KEY_H, true, emit_backspace);

    lv_obj_t *r3 = add_row(k->alpha_page, KEY_H);
    add_key(r3, k, LV_SYMBOL_UP, 15, KEY_H, true, on_shift);      /* ⇧ 大小写 */
    for (int i = 0; rows_abc[2][i]; i++)
        add_letter_key(r3, k, rows_abc[2][i]);

    lv_obj_t *r4 = add_row(k->alpha_page, KEY_H);
    add_key(r4, k, " ", 60, KEY_H, true, emit_key);               /* 空格(姓名) */
    {
        lv_obj_t *ok = add_key(r4, k, LV_SYMBOL_OK, 40, KEY_H, false, emit_ok);
        lv_obj_set_style_bg_color(ok, DG_COL_OK(), 0);
    }

    /* ---- 页脚:ABC / 123 切页 ---- */
    lv_obj_t *footer = add_row(root, 48);
    k->toggle = add_key(footer, k, "ABC", 30, 48, true, on_toggle);

    apply_layout(k);
    return root;
}
