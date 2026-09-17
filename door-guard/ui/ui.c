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

#include <string.h>

extern void page_home_register(void);
extern void page_standby_register(void);
extern void page_menu_register(void);
extern void page_users_register(void);
extern void page_device_register(void);
extern void page_access_set_register(void);
extern void page_logs_register(void);

static lv_obj_t *s_page_root = NULL;

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

    return DG_OK;
}

void ui_poll(void)
{
    display_poll();
    lv_timer_handler();
}
