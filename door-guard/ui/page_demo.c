/*
 * page_demo.c — 组件冒烟页(Phase 5;Phase 6 由真实主页替代)
 *
 * 集中触发全部 widget(按钮/四类弹窗/键盘/列表)与语言切换,
 * 模拟器人工走查 + ctest 冒烟共用。
 */
#include "dg_log.h"
#include "event_bus.h"
#include "i18n.h"
#include "page_mgr.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_kbd.h"
#include "widgets/dg_list.h"
#include "widgets/dg_popup.h"

#include <string.h>

static event_subscription_t *s_refresh_sub = NULL;

static void on_success(lv_event_t *e)
{
    (void)e;
    dg_popup_success(_("验证成功"), 1500, NULL, NULL);
}

static void on_fail(lv_event_t *e)
{
    (void)e;
    dg_popup_fail(_("验证失败"), 1500, NULL, NULL);
}

static void on_input(lv_event_t *e)
{
    (void)e;
    dg_popup_input(_("请输入密码"), true, NULL, NULL, NULL);
}

static void on_choice_pick(void *ud, int idx)
{
    (void)ud;
    DG_LOGI("[DEMO]", "choice picked idx=%d", idx);
}

static void on_choice(lv_event_t *e)
{
    (void)e;
    static const char *const opts[] = { "zh-CN", "en-US" };
    dg_popup_choice(_("语言"), opts, 2, on_choice_pick, NULL, NULL);
}

static void on_lang_switch(lv_event_t *e)
{
    (void)e;
    i18n_set_language(strcmp(i18n_current_language(), "zh-CN") == 0 ? "en-US"
                                                                    : "zh-CN");
}

/* 语言切换:静态文本重刷(EVENT_UI_REFRESH_REQUEST 页面侧标准姿势) */
static int on_refresh_evt(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    page_mgr_open(page_mgr_current());      /* demo 页:整页重建即完成刷新 */
    return 0;
}

static void on_row_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    DG_LOGI("[DEMO]", "list row clicked: %s",
            lv_label_get_text(lv_obj_get_child(row, 0)));
}

void page_demo_create(lv_obj_t *parent)
{
    DG_LOGI("[DEMO]", "page create");
    s_refresh_sub = event_bus_subscribe(EVENT_UI_REFRESH_REQUEST, on_refresh_evt, NULL);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, DG_PAD, 0);
    lv_obj_set_style_pad_row(parent, 12, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("门禁设置"));
    lv_obj_set_style_text_font(title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *b1 = dg_btn_create(row, LV_SYMBOL_OK, _("验证"));
    lv_obj_set_width(b1, LV_PCT(48));
    lv_obj_add_event_cb(b1, on_success, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = dg_btn_create(row, LV_SYMBOL_CLOSE, _("验证失败"));
    lv_obj_set_width(b2, LV_PCT(48));
    lv_obj_add_event_cb(b2, on_fail, LV_EVENT_CLICKED, NULL);

    lv_obj_t *row2 = lv_obj_create(parent);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row2, 12, 0);

    lv_obj_t *b3 = dg_btn_create(row2, LV_SYMBOL_KEYBOARD, _("请输入密码"));
    lv_obj_set_width(b3, LV_PCT(48));
    lv_obj_add_event_cb(b3, on_input, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b4 = dg_btn_create(row2, LV_SYMBOL_SETTINGS, _("语言"));
    lv_obj_set_width(b4, LV_PCT(48));
    lv_obj_add_event_cb(b4, on_choice, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lang_btn = dg_btn_create_light(parent, LV_SYMBOL_REFRESH,
                                             _("语言") );
    lv_obj_add_event_cb(lang_btn, on_lang_switch, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = dg_list_create(parent);
    lv_obj_set_height(list, 300);
    dg_list_add_row(list, LV_SYMBOL_LIST, _("记录查询"), on_row_click);
    dg_list_add_row(list, LV_SYMBOL_WIFI, _("网络配置"), on_row_click);
    dg_list_add_row(list, LV_SYMBOL_EDIT, _("编辑"), on_row_click);
}

void page_demo_destroy(void)
{
    if (s_refresh_sub) {
        event_bus_unsubscribe(s_refresh_sub);
        s_refresh_sub = NULL;
    }
    DG_LOGI("[DEMO]", "page destroy");
}

void page_demo_register(void)
{
    static const dg_page_ops_t ops = {
        .name = "demo",
        .create = page_demo_create,
        .destroy = page_demo_destroy,
    };
    page_mgr_register(&ops);
}
