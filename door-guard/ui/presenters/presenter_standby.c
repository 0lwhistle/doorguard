/*
 * presenter_standby.c — 待机页展示器:注册页面(唤醒链路:触摸→bridge_touch
 * →EV_UI_TOUCH→FSM→EV_UI_GOTO_PAGE→navigator_switch 回 home)
 */
#include "presenter_standby.h"
#include "navigator/navigator.h"

extern void page_standby_create(lv_obj_t *parent);
extern void page_standby_destroy(void);

void presenter_standby_register(void)
{
    static const navigator_page_t ops = {
        .name = "standby",
        .create = page_standby_create,
        .destroy = page_standby_destroy,
    };
    navigator_register(&ops);
}
