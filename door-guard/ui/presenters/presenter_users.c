/*
 * presenter_users.c — users 页展示器:注册页面(里程碑1:视图内聚,动作仍经 bridge)
 */
#include "presenter_users.h"
#include "navigator/navigator.h"

extern void page_users_create(lv_obj_t *parent);
extern void page_users_destroy(void);

void presenter_users_register(void)
{
    static const navigator_page_t ops = {
        .name = "users",
        .create = page_users_create,
        .destroy = page_users_destroy,
    };
    navigator_register(&ops);
}
