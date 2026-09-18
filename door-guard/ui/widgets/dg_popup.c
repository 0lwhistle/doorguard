/*
 * dg_popup.c — 统一弹窗实现(lv_layer_top 顶层;单实例管理)
 *
 * 单弹窗约束(spec-auth-business §5)使静态上下文安全:同一时刻至多一个
 * 弹窗、一类回调上下文。自动关闭定时器记在弹窗 user_data,手动关闭路径
 * 先删定时器防悬挂。
 */
#include "dg_popup.h"
#include "dg_btn.h"
#include "dg_kbd.h"
#include "../i18n.h"
#include "../theme.h"
#include "dg_log.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_popup = NULL;      /* 当前弹窗遮罩(单实例) */

static void close_internal(void)
{
    if (s_popup) {
        lv_timer_t *t = lv_obj_get_user_data(s_popup);
        if (t)
            lv_timer_del(t);            /* 悬挂定时器先清(一次性定时器触发中不受影响) */
        lv_obj_del(s_popup);
        s_popup = NULL;
    }
}

void dg_popup_close(void)
{
    close_internal();
}

bool dg_popup_active(void)
{
    return s_popup != NULL;
}

/* 基础容器:全屏遮罩 + 描边卡片,返回卡片 */
static lv_obj_t *base_create(lv_color_t accent)
{
    close_internal();                   /* 新弹窗顶掉旧弹窗 */

    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, DG_RADIUS * 2, 0);
    lv_obj_set_style_bg_color(card, DG_COL_BG(), 0);
    lv_obj_set_style_border_width(card, 6, 0);
    lv_obj_set_style_border_color(card, accent, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);

    s_popup = mask;
    return card;
}

static lv_obj_t *msg_create(lv_obj_t *card, const char *text)
{
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, DG_COL_TEXT(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    return lbl;
}

static lv_obj_t *icon_create(lv_obj_t *card, const char *sym, lv_color_t color)
{
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_color(icon, color, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);  /* 大符号 */
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(icon, LV_PCT(100));
    return icon;
}

/* ---- 结果弹窗(成功/失败共用) ---- */

typedef struct {
    void (*on_close)(void *);
    void *ud;
} result_ctx_t;

static result_ctx_t s_result;

static void result_timer_cb(lv_timer_t *t)
{
    (void)t;
    void (*cb)(void *) = s_result.on_close;
    void *ud = s_result.ud;
    if (s_popup) {
        lv_obj_del(s_popup);    /* 一次性定时器回调后由 LVGL 自删,勿在 del 路径再删 */
        s_popup = NULL;
    }
    if (cb)
        cb(ud);
}

static void result_popup(const char *text, const char *sym, lv_color_t color,
                         const char *level, uint32_t timeout_ms,
                         void (*on_close)(void *), void *ud)
{
    lv_obj_t *card = base_create(color);
    icon_create(card, sym, color);
    msg_create(card, text);
    s_result.on_close = on_close;
    s_result.ud = ud;

    lv_timer_t *t = lv_timer_create(result_timer_cb, timeout_ms, card);
    lv_timer_set_repeat_count(t, 1);
    lv_obj_set_user_data(s_popup, t);
    DG_LOGI("[POPUP]", "%s: %s", level, text);
}

void dg_popup_success(const char *text, uint32_t timeout_ms,
                      void (*on_close)(void *), void *ud)
{
    result_popup(text, LV_SYMBOL_OK, DG_COL_OK(), "success", timeout_ms, on_close, ud);
}

void dg_popup_fail(const char *text, uint32_t timeout_ms,
                   void (*on_close)(void *), void *ud)
{
    result_popup(text, LV_SYMBOL_CLOSE, DG_COL_ERR(), "fail", timeout_ms, on_close, ud);
}

/* ---- 输入弹窗 ---- */

#define INPUT_BUF_MAX 64

typedef struct {
    lv_obj_t *ta;
    lv_obj_t *err;                       /* 合法性提示行(红字,默认隐藏) */
    const char *(*validate)(const char *);
    void (*on_confirm)(void *, const char *);
    void (*on_cancel)(void *);
    void *ud;
    char buf[INPUT_BUF_MAX];
    uint16_t max_len;
} input_ctx_t;

static input_ctx_t s_input;

static void input_sync(void)
{
    lv_textarea_set_text(s_input.ta, s_input.buf);
}

/* 用户重新编辑 → 收掉上一次的错误提示(不让旧报错留在屏上) */
static void input_err_clear(void)
{
    if (s_input.err)
        lv_obj_add_flag(s_input.err, LV_OBJ_FLAG_HIDDEN);
}

static void input_key(void *ud, const char *sym)
{
    (void)ud;
    size_t len = strlen(s_input.buf);
    size_t cap = s_input.max_len ? s_input.max_len : (INPUT_BUF_MAX - 1);
    if (cap > INPUT_BUF_MAX - 1)
        cap = INPUT_BUF_MAX - 1;
    if (len + strlen(sym) <= cap)
        strcat(s_input.buf, sym);
    input_sync();
    input_err_clear();
}

static void input_backspace(void *ud)
{
    (void)ud;
    size_t len = strlen(s_input.buf);
    if (len > 0)
        s_input.buf[len - 1] = '\0';
    input_sync();
    input_err_clear();
}

static void input_ok(void *ud)
{
    (void)ud;
    /* 合法性检测(spec-ui §6):不合格就地红字提示并保持弹窗,不提交 */
    if (s_input.validate) {
        const char *err = s_input.validate(s_input.buf);
        if (err) {
            if (s_input.err) {
                lv_label_set_text(s_input.err, err);
                lv_obj_clear_flag(s_input.err, LV_OBJ_FLAG_HIDDEN);
            }
            DG_LOGI("[POPUP]", "input rejected: %s", err);
            return;
        }
    }
    char buf[INPUT_BUF_MAX];
    snprintf(buf, sizeof(buf), "%s", s_input.buf);
    void (*cb)(void *, const char *) = s_input.on_confirm;
    void *ud2 = s_input.ud;
    dg_popup_close();                   /* 先关再回调:回调可立即开新弹窗 */
    if (cb)
        cb(ud2, buf);
}

static void input_cancel_click(lv_event_t *e)
{
    void (*cb)(void *) = lv_event_get_user_data(e);
    void *ud2 = s_input.ud;
    dg_popup_close();
    if (cb)
        cb(ud2);
}

void dg_popup_input(const dg_popup_input_cfg_t *cfg)
{
    if (!cfg || !cfg->title)
        return;

    lv_obj_t *card = base_create(DG_COLOR_PRIM());
    /* 输入弹窗要放下键盘(数字 4 行 / 字母 4 行 + 页脚),卡片放宽到 660 */
    lv_obj_set_width(card, 660);
    s_input.validate = cfg->validate;
    s_input.on_confirm = cfg->on_confirm;
    s_input.on_cancel = cfg->on_cancel;
    s_input.ud = cfg->ud;
    s_input.max_len = cfg->max_len;
    s_input.buf[0] = '\0';

    msg_create(card, cfg->title);

    s_input.ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_input.ta, true);
    lv_textarea_set_password_mode(s_input.ta, cfg->mask_text);
    lv_textarea_set_text(s_input.ta, "");
    lv_textarea_set_max_length(s_input.ta,
                               cfg->max_len ? cfg->max_len : INPUT_BUF_MAX - 1);
    lv_obj_set_style_text_font(s_input.ta, DG_FONT_CN, 0);
    lv_obj_set_width(s_input.ta, LV_PCT(100));
    lv_obj_clear_flag(s_input.ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    /* 合法性提示行:默认隐藏,校验失败时红字显示(spec-ui §6) */
    s_input.err = lv_label_create(card);
    lv_label_set_text(s_input.err, "");
    lv_obj_set_style_text_font(s_input.err, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_input.err, DG_COL_ERR(), 0);
    lv_obj_set_width(s_input.err, LV_PCT(100));
    lv_label_set_long_mode(s_input.err, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_input.err, LV_OBJ_FLAG_HIDDEN);

    dg_kbd_ops_t ops = {
        .on_key = input_key,
        .on_backspace = input_backspace,
        .on_ok = input_ok,
        .user_data = NULL,
    };
    dg_kbd_create(card, cfg->start_alpha, &ops);

    lv_obj_t *cancel = dg_btn_create_light(card, LV_SYMBOL_CLOSE, _("取消"));
    lv_obj_add_event_cb(cancel, input_cancel_click, LV_EVENT_CLICKED, cfg->on_cancel);
    DG_LOGI("[POPUP]", "input: %s", cfg->title);
}

/* ---- 选择弹窗 ---- */

typedef struct {
    void (*on_pick)(void *, int);
    void (*on_cancel)(void *);
    void *ud;
} choice_ctx_t;

static choice_ctx_t s_choice;

static void choice_pick(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    void (*cb)(void *, int) = s_choice.on_pick;
    void *ud = s_choice.ud;
    dg_popup_close();
    if (cb)
        cb(ud, idx);
}

void dg_popup_choice(const char *title, const char *const *options, int cnt,
                     void (*on_pick)(void *ud, int idx),
                     void (*on_cancel)(void *ud), void *ud)
{
    lv_obj_t *card = base_create(DG_COLOR_PRIM());
    msg_create(card, title);
    s_choice.on_pick = on_pick;
    s_choice.on_cancel = on_cancel;
    s_choice.ud = ud;

    for (int i = 0; i < cnt; i++) {
        lv_obj_t *btn = dg_btn_create(card, NULL, options[i]);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, choice_pick, LV_EVENT_CLICKED, NULL);
    }
    DG_LOGI("[POPUP]", "choice: %s (%d 项)", title, cnt);
}
