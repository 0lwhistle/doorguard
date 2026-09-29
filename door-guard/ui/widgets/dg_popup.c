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
            lv_timer_delete(t);            /* 悬挂定时器先清(一次性定时器触发中不受影响) */
        lv_obj_delete(s_popup);
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
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);   /* 60%:卡片更聚焦(50% 偏飘) */
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, DG_RADIUS * 2, 0);
    lv_obj_set_style_bg_color(card, DG_COL_BG(), 0);
    /* 描边 3px(原 6px 喧宾夺主):卡片本身浮在暗遮罩上已有层次,彩色描边
     * 只做状态点缀(成功绿/失败红/输入蓝) */
    lv_obj_set_style_border_width(card, 3, 0);
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
        lv_obj_delete(s_popup);    /* 一次性定时器回调后由 LVGL 自删,勿在 del 路径再删 */
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
    void (*format)(char *buf, size_t cap);
    void (*on_confirm)(void *, const char *);
    void (*on_cancel)(void *);
    void *ud;
    char buf[INPUT_BUF_MAX];
    uint16_t max_len;
    uint32_t pos;                        /* 光标 = 字符索引(UTF-8,与 textarea 同单位) */
} input_ctx_t;

static input_ctx_t s_input;

/* ---- UTF-8 光标定位(键盘键值全 ASCII,但预填可能是中文姓名/web 录入值) ---- */

/* 字符起点 = 非 continuation 字节(0x10xxxxxx) */
static bool utf8_is_start(unsigned char c)
{
    return (c & 0xC0) != 0x80;
}

/* 字符索引 → 字节偏移;越界时停在串尾(夹到末尾语义)。
 * 数到目标字符起点后必须吃掉它自身的 continuation 字节,否则多字节字符
 * (中文)会落在字符中间——ASCII 每字节都是起点,恰好掩盖了这个错 */
static uint32_t buf_byte_of_char(const char *s, uint32_t chars)
{
    uint32_t byte = 0, seen = 0;
    while (seen < chars && s[byte]) {
        seen += utf8_is_start((unsigned char)s[byte]);
        byte++;
    }
    while (s[byte] && !utf8_is_start((unsigned char)s[byte]))
        byte++;
    return byte;
}

/* 字节偏移处向前一个字符的起点(byte>0 才有意义) */
static uint32_t buf_prev_char_begin(const char *s, uint32_t byte)
{
    while (byte > 0 && !utf8_is_start((unsigned char)s[byte - 1]))
        byte--;                          /* 先跳过 continuation 字节 */
    return byte > 0 ? byte - 1 : 0;
}

static uint32_t buf_char_count(const char *s)
{
    uint32_t n = 0;
    for (; *s; s++)
        n += utf8_is_start((unsigned char)*s);
    return n;
}

/* 光标统一从 textarea 读回:点击输入框/◀▶ 由 LVGL 摆光标,它是权威;
 * 越界值(点击空白 = CURSOR_LAST)夹到末尾 */
static void input_pos_reload(void)
{
    uint32_t cnt = buf_char_count(s_input.buf);
    uint32_t pos = lv_textarea_get_cursor_pos(s_input.ta);
    s_input.pos = pos > cnt ? cnt : pos;
}

/* 缓冲变化后:跑格式化钩子(IP 点号固定重排)→ 同步 textarea → 摆回光标 */
static void input_sync(void)
{
    if (s_input.format)
        s_input.format(s_input.buf, sizeof(s_input.buf));
    lv_textarea_set_text(s_input.ta, s_input.buf);
    /* set_text 会把光标清零;format 重排后字符数可能变,越界夹到末尾 */
    uint32_t cnt = buf_char_count(s_input.buf);
    lv_textarea_set_cursor_pos(s_input.ta,
                               s_input.pos > cnt ? cnt : s_input.pos);
}

/* 用户重新编辑 → 收掉上一次的错误提示(不让旧报错留在屏上) */
static void input_err_clear(void)
{
    if (s_input.err)
        lv_obj_add_flag(s_input.err, LV_OBJ_FLAG_HIDDEN);
}

/* 键入:插在光标处(点击定位后 = 中插),光标随之前移 */
static void input_key(void *ud, const char *sym)
{
    (void)ud;
    input_pos_reload();
    size_t len = strlen(s_input.buf);
    size_t cap = s_input.max_len ? s_input.max_len : (INPUT_BUF_MAX - 1);
    if (cap > INPUT_BUF_MAX - 1)
        cap = INPUT_BUF_MAX - 1;
    size_t sym_len = strlen(sym);
    if (len + sym_len > cap) {
        input_sync();                    /* 满了丢弃,维持可继续点删除的原语义 */
        return;
    }
    size_t at = buf_byte_of_char(s_input.buf, s_input.pos);
    memmove(s_input.buf + at + sym_len, s_input.buf + at, len - at + 1);
    memcpy(s_input.buf + at, sym, sym_len);
    s_input.pos++;
    input_sync();
    input_err_clear();
}

/* 删除:删光标前一个完整 UTF-8 字符(修复旧实现按字节删,中文姓名
 * 删出半个字符乱码的隐患) */
static void input_backspace(void *ud)
{
    (void)ud;
    input_pos_reload();
    if (s_input.pos == 0)
        return;
    uint32_t at = buf_byte_of_char(s_input.buf, s_input.pos);
    uint32_t begin = buf_prev_char_begin(s_input.buf, at);
    size_t len = strlen(s_input.buf);
    memmove(s_input.buf + begin, s_input.buf + at, len - at + 1);
    s_input.pos--;
    input_sync();
    input_err_clear();
}

/* ◀▶:只动 textarea 光标,不动缓冲;下次键入/删除前 input_pos_reload 会取回 */
static void input_cursor_left(void *ud)
{
    (void)ud;
    lv_textarea_cursor_left(s_input.ta);
}

static void input_cursor_right(void *ud)
{
    (void)ud;
    lv_textarea_cursor_right(s_input.ta);
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
    s_input.format = cfg->format;
    s_input.on_confirm = cfg->on_confirm;
    s_input.on_cancel = cfg->on_cancel;
    s_input.ud = cfg->ud;
    s_input.max_len = cfg->max_len;
    /* 预填(编辑现值):同步进键盘缓冲与 textarea,确认取到的才是完整文本;
     * 带格式钩子时预填也过一遍(如点号固定输入的补零形态) */
    if (cfg->initial && cfg->initial[0])
        snprintf(s_input.buf, sizeof(s_input.buf), "%s", cfg->initial);
    else
        s_input.buf[0] = '\0';
    if (s_input.format)
        s_input.format(s_input.buf, sizeof(s_input.buf));

    msg_create(card, cfg->title);

    s_input.ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_input.ta, true);
    lv_textarea_set_password_mode(s_input.ta, cfg->mask_text);
    lv_textarea_set_max_length(s_input.ta,
                               cfg->max_len ? cfg->max_len : INPUT_BUF_MAX - 1);
    lv_obj_set_style_text_font(s_input.ta, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_input.ta, DG_COL_TEXT(), 0);
    /* 输入框本体:白底蓝框。产品不初始化 lv_theme,textarea 默认无样式,
     * 此前就是「无框浮字」;框出来输入区在哪一目了然 */
    lv_obj_set_style_bg_color(s_input.ta, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(s_input.ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_input.ta, 2, 0);
    lv_obj_set_style_border_color(s_input.ta, DG_COLOR_PRIM(), 0);
    lv_obj_set_style_radius(s_input.ta, DG_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_input.ta, 16, 0);
    lv_obj_set_style_pad_ver(s_input.ta, 12, 0);
    /* 光标:主蓝块 + 反白字符。无主题时 LV_PART_CURSOR 零样式 = 画出透明矩形
     * 看不见;块状比细竖线在触摸屏上更显眼 */
    lv_obj_set_style_bg_color(s_input.ta, DG_COLOR_PRIM(), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_input.ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_text_color(s_input.ta, DG_COL_BG(), LV_PART_CURSOR);
    lv_obj_set_width(s_input.ta, LV_PCT(100));
    /* 点击输入框即定位光标(LVGL cursor.click_pos 默认开启,不依赖聚焦) */
    lv_obj_clear_flag(s_input.ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    s_input.pos = buf_char_count(s_input.buf);   /* 预填时光标停在末尾 */
    input_sync();                                /* set_text 后把光标摆到位 */

    /* 合法性提示行:默认隐藏,校验失败时红字显示(spec-ui §6) */
    s_input.err = lv_label_create(card);
    lv_label_set_text(s_input.err, "");
    lv_obj_set_style_text_font(s_input.err, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_input.err, DG_COL_ERR(), 0);
    lv_obj_set_width(s_input.err, LV_PCT(100));
    lv_label_set_long_mode(s_input.err, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_input.err, LV_OBJ_FLAG_HIDDEN);

    dg_kbd_ops_t ops = {
        .on_key = input_key,
        .on_backspace = input_backspace,
        .on_ok = input_ok,
        .on_cursor_left = input_cursor_left,
        .on_cursor_right = input_cursor_right,
        .user_data = NULL,
    };
    dg_kbd_create(card, cfg->start_alpha, &ops);

    lv_obj_t *cancel = dg_btn_create_light(card, NULL, _("取消"));
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

static void choice_cancel_click(lv_event_t *e)
{
    void (*cb)(void *) = lv_event_get_user_data(e);
    void *ud = s_choice.ud;
    dg_popup_close();
    if (cb)
        cb(ud);
}

void dg_popup_choice(const char *title, const char *const *options, int cnt,
                     void (*on_pick)(void *ud, int idx),
                     void (*on_cancel)(void *ud), void *ud)
{
    dg_popup_choice_ex(title, options, cnt, 0, on_pick, on_cancel, ud);
}

void dg_popup_choice_ex(const char *title, const char *const *options, int cnt,
                        uint32_t red_mask, void (*on_pick)(void *ud, int idx),
                        void (*on_cancel)(void *ud), void *ud)
{
    lv_obj_t *card = base_create(DG_COLOR_PRIM());
    msg_create(card, title);
    s_choice.on_pick = on_pick;
    s_choice.on_cancel = on_cancel;
    s_choice.ud = ud;

    for (int i = 0; i < cnt; i++) {
        /* red_mask 置位的选项 = 破坏性动作(删除/清除),红底白字标示 */
        lv_obj_t *btn = (red_mask & (1u << i)) ? dg_btn_create_danger(card, NULL, options[i])
                                               : dg_btn_create(card, NULL, options[i]);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, choice_pick, LV_EVENT_CLICKED, NULL);
    }
    /* 取消按钮:此前 on_cancel 回调存了却没有任何入口能触发——
     * 用户点错必须硬着头皮选一项,没有反悔权(2026-09-21 用户反馈) */
    lv_obj_t *cancel = dg_btn_create_light(card, NULL, _("取消"));
    lv_obj_add_event_cb(cancel, choice_cancel_click, LV_EVENT_CLICKED,
                        s_choice.on_cancel);
    DG_LOGI("[POPUP]", "choice: %s (%d 项, red=%#x)", title, cnt, red_mask);
}
