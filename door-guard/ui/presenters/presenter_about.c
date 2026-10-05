/*
 * presenter_about.c — 关于设备页展示器
 *
 * on_evt 把 OTA 状态拍与 broker 连接态渲染进页面;触发动作由页面直调
 * ota_update(与 net_set 页「页面直发总线」同款分工)。
 */
#include "presenter_about.h"
#include "navigator/navigator.h"
#include "pages/page_about.h"
#include "ui_events.h"

static void about_on_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_OTA_STATUS:
        page_about_on_result(&evt->ota);
        break;
    case UI_EVT_MQTT_STATE:
        page_about_on_mqtt(evt->mqtt_state.connected);
        break;
    default:
        break;
    }
}

void presenter_about_register(void)
{
    static const navigator_page_t ops = {
        .name = "about",
        .create = page_about_create,
        .destroy = page_about_destroy,
        .on_evt = about_on_evt,
    };
    navigator_register(&ops);
}
