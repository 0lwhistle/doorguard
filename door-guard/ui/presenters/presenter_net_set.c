/*
 * presenter_net_set.c — 网络配置页展示器
 *
 * on_evt 把 net 回执(EV_NET_CFG_RESULT 经 bridge 入队)渲染成页面弹窗;
 * 应用请求由页面直接发总线(与设备管理页 NTP 触发同款)。
 */
#include "presenter_net_set.h"
#include "i18n.h"
#include "pages/page_net_set.h"
#include "widgets/dg_popup.h"

static void net_set_on_evt(const ui_evt_t *evt)
{
    if (evt->kind != UI_EVT_NET_CFG_RESULT)
        return;
    page_net_set_on_result(evt->net_cfg_result.ok,
                           evt->net_cfg_result.err,
                           evt->net_cfg_result.ip);
}

void presenter_net_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "net_set",
        .create = page_net_set_create,
        .destroy = page_net_set_destroy,
        .on_evt = net_set_on_evt,
    };
    navigator_register(&ops);
}
