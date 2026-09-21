/*
 * page_web_set.c — Web 管理页(spec-ui §3.3 设备管理 + spec-network §1)
 *
 * 内容:web 服务状态与局域网访问地址(只读显示)、当前账号、改账号/改口令。
 *
 * 为什么口令要输两次:口令是掩码输入,且这是**远程管理入口**——打错一位
 * 就再也登不进上位机(只能到设备上重设)。二次确认把“手滑”挡在保存之前。
 *
 * 动作一律经 bridge 发事件(net 模块落地),失败原因由 presenter 弹窗显示;
 * 本页不发业务判断,也不直接读凭据。
 */
#include "page_web_set.h"
#include "bridge/bridge.h"
#include "dg_log.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "valid_ui.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>

#define STATUS_Y   96
#define INFO_Y     210
#define BTN_Y      380

static lv_obj_t *s_status = NULL;
static lv_obj_t *s_url = NULL;
static lv_obj_t *s_user = NULL;
static lv_obj_t *s_warn = NULL;

/* 待改的账号:空串 = 保持当前账号(只改口令) */
static char s_new_user[32] = "";
static char s_first_pwd[32] = "";
static int  s_pending = 0;                 /* 0 无 / 1 等新口令 / 2 等确认口令 */

/* ---- 口令二次确认流程 ---- */

static void ask_pwd_confirm(void);

static void on_pwd_first(void *ud, const char *text)
{
    (void)ud;
    snprintf(s_first_pwd, sizeof(s_first_pwd), "%s", text);
    s_pending = 2;
    ask_pwd_confirm();
}

static void on_pwd_second(void *ud, const char *text)
{
    (void)ud;
    s_pending = 0;
    if (strcmp(s_first_pwd, text) != 0) {
        dg_popup_fail(_("两次输入的密码不一致"), 2000, NULL, NULL);
        memset(s_first_pwd, 0, sizeof(s_first_pwd));
        return;
    }
    /* 提交给 net 模块(net 侧再跑一遍合法性 + 落库) */
    bridge_web_set(s_new_user, s_first_pwd);
    memset(s_first_pwd, 0, sizeof(s_first_pwd));
    dg_popup_success(_("正在保存…"), 800, NULL, NULL);
}

static void on_popup_cancel(void *ud)
{
    (void)ud;
    s_pending = 0;
    memset(s_first_pwd, 0, sizeof(s_first_pwd));
    memset(s_new_user, 0, sizeof(s_new_user));
}

static void ask_pwd_confirm(void)
{
    dg_popup_input_cfg_t cfg = {
        .title = _("请再次输入新密码"),
        .mask_text = true,
        .start_alpha = true,               /* 口令常含字母 */
        .max_len = 31,
        .validate = dg_ui_valid_pwd,
        .on_confirm = on_pwd_second,
        .on_cancel = on_popup_cancel,
    };
    dg_popup_input(&cfg);
}

static void ask_pwd(void)
{
    s_pending = 1;
    dg_popup_input_cfg_t cfg = {
        .title = _("请输入新密码"),
        .mask_text = true,
        .start_alpha = true,
        .max_len = 31,
        .validate = dg_ui_valid_pwd,
        .on_confirm = on_pwd_first,
        .on_cancel = on_popup_cancel,
    };
    dg_popup_input(&cfg);
}

/* ---- 按钮 ---- */

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

/* 改账号:先输账号,再走口令二次确认(账号与口令一起保存——
 * 存储层一次写入,避免“账号改了口令没改”的中间态) */
static void on_edit_user(void *ud, const char *text)
{
    (void)ud;
    snprintf(s_new_user, sizeof(s_new_user), "%s", text);
    ask_pwd();
}

static void on_edit_user_click(lv_event_t *e)
{
    (void)e;
    dg_popup_input_cfg_t cfg = {
        .title = _("请输入新账号"),
        .mask_text = false,
        .start_alpha = true,
        .max_len = 31,
        .validate = dg_ui_valid_uid,
        .on_confirm = on_edit_user,
        .on_cancel = on_popup_cancel,
    };
    dg_popup_input(&cfg);
}

/* 只改口令:账号保持不变(空串 = net 侧沿用当前账号) */
static void on_edit_pwd_click(lv_event_t *e)
{
    (void)e;
    s_new_user[0] = '\0';
    ask_pwd();
}

/* ---- 页面 ---- */

static lv_obj_t *make_line(lv_obj_t *parent, const char *prefix, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, prefix);
    lv_obj_set_style_text_font(l, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(l, DG_COL_TEXT(), 0);
    lv_obj_set_width(l, DG_SCREEN_W - 2 * DG_PAD);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

void page_web_set_create(lv_obj_t *parent)
{
    DG_LOGI("[WEBSET]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("Web 管理"));
    lv_obj_set_style_text_font(title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    s_status = make_line(parent, "", STATUS_Y);
    s_url = make_line(parent, "", STATUS_Y + 50);
    s_user = make_line(parent, "", INFO_Y + 40);
    s_warn = make_line(parent, "", INFO_Y + 90);
    lv_obj_set_style_text_color(s_warn, DG_COL_ERR(), 0);

    lv_obj_t *b_user = dg_btn_create(parent, NULL, _("修改账号"));
    lv_obj_set_size(b_user, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(b_user, LV_ALIGN_TOP_MID, 0, BTN_Y);
    lv_obj_add_event_cb(b_user, on_edit_user_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_pwd = dg_btn_create(parent, NULL, _("修改密码"));
    lv_obj_set_size(b_pwd, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(b_pwd, LV_ALIGN_TOP_MID, 0, BTN_Y + DG_BTN_H + DG_PAD);
    lv_obj_add_event_cb(b_pwd, on_edit_pwd_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = make_line(parent, _("账号 3~31 位，密码 4~31 位；修改后上位机需重新登录"),
                               BTN_Y + 2 * (DG_BTN_H + DG_PAD) + 10);
    lv_obj_set_style_text_color(hint, DG_COL_TEXT(), 0);

    lv_obj_t *back = dg_btn_create_light(parent, NULL, _("返回"));
    lv_obj_set_size(back, 200, DG_BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -DG_PAD);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* 页面刚打开时先请求一次状态(避免显示空) */
    bridge_web_state_req();
}

void page_web_set_destroy(void)
{
    DG_LOGI("[WEBSET]", "page destroy");
    s_status = s_url = s_user = s_warn = NULL;
    s_pending = 0;
    memset(s_first_pwd, 0, sizeof(s_first_pwd));
    memset(s_new_user, 0, sizeof(s_new_user));
}

void page_web_set_show(bool running, const char *url, const char *user,
                       bool pwd_default)
{
    char buf[160];
    if (s_status) {
        snprintf(buf, sizeof(buf), "%s%s", _("服务状态："),
                 running ? _("运行中") : _("未运行"));
        lv_label_set_text(s_status, buf);
    }
    if (s_url) {
        snprintf(buf, sizeof(buf), "%s%s", _("局域网访问地址："),
                 (url && url[0]) ? url : _("未获取到地址"));
        lv_label_set_text(s_url, buf);
    }
    if (s_user) {
        snprintf(buf, sizeof(buf), "%s%s", _("当前账号："),
                 (user && user[0]) ? user : "-");
        lv_label_set_text(s_user, buf);
    }
    if (s_warn)
        lv_label_set_text(s_warn, pwd_default ? _("默认口令，请尽快修改") : "");
}
