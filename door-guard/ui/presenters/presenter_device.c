/*
 * presenter_device.c — device 页展示器:注册页面(里程碑1:视图内聚,动作仍经 bridge)
 */
#include "presenter_device.h"
#include "navigator/navigator.h"

extern void page_device_create(lv_obj_t *parent);
extern void page_device_destroy(void);

void presenter_device_register(void)
{
    static const navigator_page_t ops = {
        .name = "device",
        .create = page_device_create,
        .destroy = page_device_destroy,
    };
    navigator_register(&ops);
}
