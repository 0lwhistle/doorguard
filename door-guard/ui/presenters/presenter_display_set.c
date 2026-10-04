/*
 * presenter_display_set.c — 屏幕显示设置页展示器:注册页面(无异步回执,
 * 滑条页内闭环,不需要 on_evt)
 */
#include "presenter_display_set.h"
#include "navigator/navigator.h"

extern void page_display_set_create(lv_obj_t *parent);
extern void page_display_set_destroy(void);

void presenter_display_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "display_set",
        .create = page_display_set_create,
        .destroy = page_display_set_destroy,
    };
    navigator_register(&ops);
}
