/*
 * presenter_device_data.c — 设备数据页展示器
 *
 * 二次确认与结果弹窗都在页面里闭环(on_pick 发 op、on_result 配对回执);
 * 展示器只把 UI_EVT_DATA_OP_RESULT 转给页面——与 about 页同款分工。
 */
#include "presenter_device_data.h"
#include "navigator/navigator.h"
#include "pages/page_device_data.h"
#include "ui_events.h"

static void device_data_on_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_DATA_OP_RESULT:
        page_device_data_on_result(&evt->data_op);
        break;
    default:
        break;
    }
}

void presenter_device_data_register(void)
{
    static const navigator_page_t ops = {
        .name = "device_data",
        .create = page_device_data_create,
        .destroy = page_device_data_destroy,
        .on_evt = device_data_on_evt,
    };
    navigator_register(&ops);
}
