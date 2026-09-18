/*
 * presenter_web_set.c — Web 管理页展示器
 *
 * 职责(与其余 presenter 一致):
 *   on_enter  → 拉状态(发 EV_NET_WEB_STATE_REQ,结果异步回来)
 *   on_evt    → 把 net 回执渲染成页面文本/弹窗
 * 页面本身不碰后端:状态经 page_web_set_show() 推入。
 */
#include "presenter_web_set.h"
#include "err.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "pages/page_web_set.h"
#include "widgets/dg_popup.h"

#include "bridge/bridge.h"

static void web_set_on_enter(void)
{
    bridge_web_state_req();
}

static void web_set_on_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_WEB_STATE:
        page_web_set_show(evt->web_state.running, evt->web_state.url,
                          evt->web_state.user, evt->web_state.pwd_default);
        break;
    case UI_EVT_WEB_SET_RESULT:
        if (evt->web_set_result.ok)
            dg_popup_success(_("Web 账号已更新"), 2000, NULL, NULL);
        else if (evt->web_set_result.err == DG_ERR_BAD_UID)
            dg_popup_fail(_("账号不合法：需 3~31 位字母、数字、- 或 _"), 2500, NULL, NULL);
        else if (evt->web_set_result.err == DG_ERR_BAD_PWD)
            dg_popup_fail(_("密码不合法：需 4~31 位，不能含空格"), 2500, NULL, NULL);
        else
            dg_popup_fail(_("Web 账号更新失败"), 2500, NULL, NULL);
        bridge_web_state_req();          /* 失败后刷新显示,状态与设备保持一致 */
        break;
    default:
        break;
    }
}

void presenter_web_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "web_set",
        .create = page_web_set_create,
        .destroy = page_web_set_destroy,
        .on_enter = web_set_on_enter,
        .on_evt = web_set_on_evt,
    };
    navigator_register(&ops);
}
