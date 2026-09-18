/*
 * presenter_home.c — 主页展示器:验证流程弹窗编排 + 事件渲染 + 语言刷新
 *
 * 数据流:后端事件 → bridge(入队) → ui.c 泵(LVGL 线程)→ 本文件 on_evt
 * → page_home_set_* / dg_popup_*(视图不持业务状态,决策全在 access FSM)。
 * 弹窗回调反向经 bridge 发动作事件(uid/pwd/method/cancel),服务层再决策。
 */
#include "presenter_home.h"
#include "pages/page_home.h"
#include "navigator/navigator.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "ui_events.h"
#include "widgets/dg_popup.h"
#include "dg_log.h"

#include <stdio.h>
#include <string.h>

/* 密码弹窗要原样回填 uid:弹窗是异步的,故存本层静态(同一时刻只有一个弹窗) */
static char s_pwd_uid[DG_UID_LEN];

/* 方式选择弹窗的选项→枚举映射(弹窗回调只给下标) */
static int32_t s_pick_methods[4];

/* 失败原因 → 文案(spec-ui:陌生/黑名单对外统一显示「验证失败」,不泄露
 * 是否已录入;「非管理员」只出现在管理员入口,不涉及录入信息) */
static const char *result_text(const ev_ui_result_t *r)
{
    if (r->not_admin)
        return _("非管理员");
    switch (r->reason) {
    case DG_REASON_NO_USER:           return _("用户不存在");
    case DG_REASON_WRONG_PWD:         return _("密码错误");
    case DG_REASON_METHOD_DISABLED:   return _("该方式未开启");
    case DG_REASON_AUTH_DISABLED:     return _("全部验证方式已关闭");
    case DG_REASON_DEVICE_ERR:        return _("摄像头未就绪");
    default:                          return _("验证失败");   /* 陌生人/黑名单/超时… */
    }
}

/* ---- 弹窗回调 → 动作事件 ---- */

static void on_uid_confirm(void *ud, const char *text)
{
    (void)ud;
    bridge_uid_submit(text);
}

static void on_pwd_confirm(void *ud, const char *text)
{
    (void)ud;
    bridge_pwd_submit(s_pwd_uid, text);
}

static void on_method_pick(void *ud, int idx)
{
    (void)ud;
    if (idx < 0 || idx >= 4)
        return;
    bridge_method_pick(s_pick_methods[idx]);
}

/* 弹窗取消:放弃当前验证流程(FSM 决定回普通还是回管理员认证) */
static void on_popup_cancel(void *ud)
{
    (void)ud;
    bridge_cancel();
}

/* ---- 验证方式选择(spec §4.2:只列该用户开启的方式) ---- */

static void show_method_picker(uint32_t flags)
{
    const char *labels[4];
    int n = 0;

    if (flags & DG_AUTH_FACE) {
        labels[n] = _("1:1人脸");
        s_pick_methods[n++] = DG_METHOD_FACE_11;
    }
    if (flags & DG_AUTH_FINGER) {
        labels[n] = _("指纹");
        s_pick_methods[n++] = DG_METHOD_FINGER;
    }
    if (flags & DG_AUTH_PWD) {
        labels[n] = _("密码");
        s_pick_methods[n++] = DG_METHOD_PWD;
    }
    if (flags & DG_AUTH_IC) {
        labels[n] = _("IC卡");
        s_pick_methods[n++] = DG_METHOD_IC;
    }

    if (n == 0) {
        /* FSM 已按 auth_flags==0 拦过(DG_REASON_AUTH_DISABLED);兜底放弃流程 */
        DG_LOGW("[HOME]", "无可选验证方式,放弃流程");
        bridge_cancel();
        return;
    }
    if (n == 1) {
        /* 只有一种方式:直接进,不给用户多余的一次点击 */
        bridge_method_pick(s_pick_methods[0]);
        return;
    }
    dg_popup_choice(_("验证方式"), labels, n, on_method_pick, on_popup_cancel, NULL);
}

static void home_on_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_FACE_BOX:
        page_home_set_facebox(evt->box.state, evt->box.x, evt->box.y,
                              evt->box.w, evt->box.h);
        break;
    case UI_EVT_FACE_LOST:
        page_home_clear_facebox();
        break;
    case UI_EVT_FACEBOX: {
        /* 服务层(FSM)的脸框决策:绿=命中 / 红=失败 / -1=隐藏 */
        const ev_ui_facebox_t *f = &evt->facebox;
        if (f->state < 0)
            page_home_clear_facebox();
        else if (f->w > 0)
            page_home_set_facebox(f->state, f->x, f->y, f->w, f->h);
        else
            page_home_set_facebox_color(f->state);
        break;
    }
    case UI_EVT_ASK_UID:
        dg_popup_input(_("请输入用户ID"), false, on_uid_confirm, on_popup_cancel, NULL);
        break;
    case UI_EVT_INPUT_PWD:
        snprintf(s_pwd_uid, sizeof(s_pwd_uid), "%s", evt->input_req.uid);
        dg_popup_input(_("请输入密码"), true, on_pwd_confirm, on_popup_cancel, NULL);
        break;
    case UI_EVT_PICK_METHOD:
        show_method_picker(evt->methods.auth_flags);
        break;
    case UI_EVT_RESULT: {
        const ev_ui_result_t *r = &evt->result_popup;
        if (r->ok) {
            char text[DG_NAME_LEN + 16];
            snprintf(text, sizeof(text), "%s %s", _("验证成功"), r->user_name);
            dg_popup_success(text, 3000, NULL, NULL);
        } else {
            dg_popup_fail(result_text(r), 3000, NULL, NULL);
        }
        break;
    }
    case UI_EVT_HINT: {
        const ev_hint_t *h = &evt->hint;
        if (h->method == DG_HINT_LANG_RELOAD) {
            navigator_reload();              /* 整页重建(语言刷新) */
            break;
        }
        if (h->method == DG_HINT_NO_ADMIN) {
            /* 无管理员免认证进菜单的提示:此时页面已切到菜单,由菜单页弹(menu presenter) */
            break;
        }
        const char *text = NULL;
        if (h->method == DG_HINT_ADMIN_AUTH)
            text = _("管理员认证");
        else if (h->method == DG_METHOD_FACE_11)
            text = _("请正对摄像头");
        else if (h->method == DG_METHOD_FINGER)
            text = _("请按指纹");
        else if (h->method == DG_METHOD_PWD)
            text = _("请输入密码");
        else if (h->method == DG_METHOD_IC)
            text = _("请刷卡");
        page_home_set_hint(text);
        break;
    }
    case UI_EVT_HINT_CLEAR:
        page_home_set_hint(NULL);
        break;
    case UI_EVT_AUTH_RESULT:
        break;                               /* web/日志侧消费;主页文案走 EV_UI_RESULT */
    case UI_EVT_GOTO_PAGE:
        break;                               /* 切页由 ui 层泵直驱 */
    }
}

void presenter_home_register(void)
{
    static const navigator_page_t ops = {
        .name = "home",
        .create = page_home_create,
        .destroy = page_home_destroy,
        .on_evt = home_on_evt,
    };
    navigator_register(&ops);
    s_pwd_uid[0] = '\0';
    memset(s_pick_methods, 0, sizeof(s_pick_methods));
}
