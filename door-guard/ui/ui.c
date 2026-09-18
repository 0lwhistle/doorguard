/*
 * ui.c — UI 子系统引导(装配顺序学自 ESP32 ovs ui_bootstrap)
 *
 *   display → i18n → navigator(绑根) → 注册页面 → bridge(事件桥)
 *   → 全局事件泵(33ms,页面生命周期之外)→ 压栈首页
 *
 * 事件流向:
 *   后端 event_bus → bridge(入队) → 本泵(LVGL 线程) → navigator
 *   ├─ UI_EVT_GOTO_PAGE → navigator_switch(切页)
 *   └─ 其余            → navigator_dispatch_evt(当前页 presenter 渲染)
 */
#include "ui.h"
#include "cfg.h"
#include "hal/display/display.h"
#include "i18n.h"
#include "lvgl.h"
#include "navigator/navigator.h"
#include "bridge/bridge.h"
#include "port.h"
#include "theme.h"
#include "ui_events.h"

#include <string.h>

extern void presenter_home_register(void);
extern void presenter_standby_register(void);
extern void presenter_menu_register(void);
extern void presenter_users_register(void);
extern void presenter_device_register(void);
extern void presenter_access_set_register(void);
extern void presenter_logs_register(void);
extern void presenter_web_set_register(void);

static lv_obj_t *s_page_root = NULL;

/* 全局事件泵:必须活在页面生命周期之外——
 * 原实现泵在 page_home 的定时器里,进待机 → home 销毁 → 泵停 →
 * 唤醒事件永远无人处理,屏幕卡死在待机(实测) */
static void ui_evt_pump_cb(lv_timer_t *t)
{
    (void)t;
    ui_evt_t evt;
    while (ui_evt_pop(&evt)) {
        if (evt.kind == UI_EVT_GOTO_PAGE)
            navigator_switch(evt.page.page); /* 栈内回退/平级切换 */
        else
            navigator_dispatch_evt(&evt);    /* 内容事件给当前页渲染 */
    }
}

int ui_init(const dg_ui_args_t *args)
{
    lv_init();
    int rc = display_init();
    if (rc != DG_OK)
        return rc;

    const char *lang = cfg_get()->language;
    i18n_init(args ? args->lang_dir : "lang");

    /* 显式套用配置语言(init 默认 zh-CN) */
    if (lang && strcmp(lang, "zh-CN") != 0)
        i18n_set_language(lang);

    s_page_root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_page_root);
    lv_obj_set_size(s_page_root, DG_SCREEN_W, DG_SCREEN_H);
    navigator_init(s_page_root);

    presenter_home_register();
    presenter_standby_register();
    presenter_menu_register();
    presenter_users_register();
    presenter_device_register();
    presenter_access_set_register();
    presenter_logs_register();
    presenter_web_set_register();

    bridge_init();
    navigator_push("home");
    lv_timer_create(ui_evt_pump_cb, 33, NULL); /* 事件泵 30fps 跟手 */

    return DG_OK;
}

void ui_poll(void)
{
    display_poll();
    lv_timer_handler();
}
