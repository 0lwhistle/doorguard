/*
 * presenter_menu.c — menu 页展示器:注册页面(里程碑1:视图内聚,动作仍经 bridge)
 */
#include "presenter_menu.h"
#include "navigator/navigator.h"

extern void page_menu_create(lv_obj_t *parent);
extern void page_menu_destroy(void);

void presenter_menu_register(void)
{
    static const navigator_page_t ops = {
        .name = "menu",
        .create = page_menu_create,
        .destroy = page_menu_destroy,
    };
    navigator_register(&ops);
}
