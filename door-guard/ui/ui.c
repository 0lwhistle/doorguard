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

extern void page_demo_register(void);   /* demo 页(Phase 6 由真实页面替代) */

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

    page_demo_register();
    page_mgr_open("demo");

    return DG_OK;
}

void ui_poll(void)
{
    display_poll();
    lv_timer_handler();
}
