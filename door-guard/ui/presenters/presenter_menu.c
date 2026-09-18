/*
 * presenter_menu.c — menu 页展示器:注册页面 + 无管理员提示
 *
 * 菜单入口的决策全在 access FSM(spec-auth §3):无管理员时 FSM 免认证放行并
 * 发 DG_HINT_NO_ADMIN;切页事件在前,故该提示由本页承接(主页提示条只属于主页)。
 */
#include "presenter_menu.h"
#include "navigator/navigator.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "ui_events.h"
#include "widgets/dg_popup.h"
#include "dg_log.h"

extern void page_menu_create(lv_obj_t *parent);
extern void page_menu_destroy(void);

static void menu_on_evt(const ui_evt_t *evt)
{
    if (evt->kind == UI_EVT_HINT && evt->hint.method == DG_HINT_NO_ADMIN)
        dg_popup_fail(_("未设置管理员,请先添加管理员"), 3000, NULL, NULL);
}

void presenter_menu_register(void)
{
    static const navigator_page_t ops = {
        .name = "menu",
        .create = page_menu_create,
        .destroy = page_menu_destroy,
        .on_evt = menu_on_evt,
    };
    navigator_register(&ops);
}
