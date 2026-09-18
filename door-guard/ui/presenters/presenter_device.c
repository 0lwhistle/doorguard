/*
 * presenter_device.c — device 页展示器:注册页面(里程碑1:视图内聚,动作仍经 bridge)
 */
#include "presenter_device.h"
#include "navigator/navigator.h"
#include "widgets/dg_popup.h"
#include "i18n.h"
#include "err.h"

extern void page_device_create(lv_obj_t *parent);
extern void page_device_destroy(void);

/* NTP 结果(校正中→成功/失败):由设备管理页显示,文案区分联网失败 */
static void device_on_evt(const ui_evt_t *evt)
{
    if (evt->kind != UI_EVT_NTP_RESULT)
        return;
    if (evt->ntp.ok)
        dg_popup_success(_("NTP同步成功"), 1500, NULL, NULL);
    else if (evt->ntp.err == DG_ERR_NETWORK)
        dg_popup_fail(_("设备未联网"), 2000, NULL, NULL);
    else
        dg_popup_fail(_("NTP同步失败"), 2000, NULL, NULL);
}

void presenter_device_register(void)
{
    static const navigator_page_t ops = {
        .name = "device",
        .create = page_device_create,
        .destroy = page_device_destroy,
        .on_evt = device_on_evt,
    };
    navigator_register(&ops);
}
