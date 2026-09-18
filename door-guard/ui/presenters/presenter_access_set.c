/*
 * presenter_access_set.c — access_set 页展示器:注册页面(里程碑1:视图内聚,动作仍经 bridge)
 */
#include "presenter_access_set.h"
#include "navigator/navigator.h"

extern void page_access_set_create(lv_obj_t *parent);
extern void page_access_set_destroy(void);

void presenter_access_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "access_set",
        .create = page_access_set_create,
        .destroy = page_access_set_destroy,
    };
    navigator_register(&ops);
}
