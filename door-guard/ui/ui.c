/*
 * ui.c — UI 子系统引导与主循环泵
 */
#include "ui.h"
#include "cfg.h"
#include "hal/display/display.h"
#include "i18n.h"
#include "lvgl.h"
#include "page_mgr.h"
#include "port.h"
#include "theme.h"
#include "ui_events.h"
#include "event_bus.h"
#include "events.h"

#include <string.h>

extern void page_home_register(void);
extern void page_standby_register(void);
extern void page_menu_register(void);
extern void page_users_register(void);
extern void page_device_register(void);
extern void page_access_set_register(void);
extern void page_logs_register(void);

static lv_obj_t *s_page_root = NULL;

static int on_goto_page(const event_t *e, void *ud);
static void ui_evt_pump_cb(lv_timer_t *t);

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
    page_mgr_attach_root(s_page_root);

    page_home_register();
    page_standby_register();
    page_menu_register();
    page_users_register();
    page_device_register();
    page_access_set_register();
    page_logs_register();
    page_mgr_open("home");

    event_bus_subscribe(EV_UI_GOTO_PAGE, on_goto_page, NULL);
    lv_timer_create(ui_evt_pump_cb, 33, NULL);   /* 事件泵 30fps 跟手 */

    return DG_OK;
}

/* ---- 全局事件泵:必须活在页面生命周期之外 ----
 * 原实现泵在 page_home 的定时器里:进待机 → home 销毁 → 泵停 →
 * 唤醒事件(EV_UI_GOTO_PAGE)永远无人处理,屏幕卡死在待机(实测) */
static int on_goto_page(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_GOTO_PAGE;
    evt.page = *(const ev_goto_page_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static void ui_evt_pump_cb(lv_timer_t *t)
{
    (void)t;
    ui_evt_t evt;
    while (ui_evt_pop(&evt)) {
        if (evt.kind == UI_EVT_GOTO_PAGE)
            page_mgr_open(evt.page.page);   /* 切页是页面管理职责 */
        else
            page_mgr_dispatch_evt(&evt);    /* 内容事件给当前页渲染 */
    }
}

void ui_poll(void)
{
    display_poll();
    lv_timer_handler();
}
