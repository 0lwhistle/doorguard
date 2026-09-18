/*
 * presenter_logs.c — logs 页展示器:注册页面(里程碑1:视图内聚,动作仍经 bridge)
 */
#include "presenter_logs.h"
#include "navigator/navigator.h"

extern void page_logs_create(lv_obj_t *parent);
extern void page_logs_destroy(void);

void presenter_logs_register(void)
{
    static const navigator_page_t ops = {
        .name = "logs",
        .create = page_logs_create,
        .destroy = page_logs_destroy,
    };
    navigator_register(&ops);
}
