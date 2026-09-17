/*
 * page_menu.c — 菜单页(spec-ui §3.3):仅管理员可达,四入口宫格 + 返回
 */
#include "auth_fsm.h"
#include "dg_log.h"
#include "i18n.h"
#include "page_mgr.h"
#include "theme.h"
#include "widgets/dg_btn.h"

static void on_back(lv_event_t *e)
{
    (void)e;
    extern auth_fsm_t *page_home_fsm(void);
    /* 经 FSM 统一返回(状态同步,FSM 会 GOTO_PAGE home) */
    auth_fsm_handle(page_home_fsm(), FSM_EV_BACK, NULL);
}

static void on_users(lv_event_t *e)
{
    (void)e;
    page_mgr_open("users");
}
static void on_device(lv_event_t *e)
{
    (void)e;
    page_mgr_open("device");
}
static void on_access_set(lv_event_t *e)
{
    (void)e;
    page_mgr_open("access_set");
}
static void on_logs(lv_event_t *e)
{
    (void)e;
    page_mgr_open("logs");
}

void page_menu_create(lv_obj_t *parent)
{
    DG_LOGI("[MENU]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("菜单"));
    lv_obj_set_style_text_font(title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

    /* 2×2 宫格(spec §3.3) */
    struct {
        const char *icon;
        const char *label;
        void (*open)(lv_event_t *);
    } cells[] = {
        { LV_SYMBOL_SETTINGS, _("用户管理"), on_users },
        { LV_SYMBOL_WIFI,     _("设备管理"), on_device },
        { LV_SYMBOL_LIST,     _("门禁设置"), on_access_set },
        { LV_SYMBOL_EYE_OPEN, _("记录查询"), on_logs },
    };

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, DG_SCREEN_W - 2 * DG_PAD, 800);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, DG_PAD, 0);
    lv_obj_set_style_pad_row(grid, DG_PAD, 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = dg_btn_create(grid, cells[i].icon, cells[i].label);
        lv_obj_set_size(b, (DG_SCREEN_W - 3 * DG_PAD) / 2, 380);
        lv_obj_add_event_cb(b, cells[i].open, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 200, DG_BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -DG_PAD);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
}

void page_menu_destroy(void)
{
    DG_LOGI("[MENU]", "page destroy");
}

void page_menu_register(void)
{
    static const dg_page_ops_t ops = {
        .name = "menu",
        .create = page_menu_create,
        .destroy = page_menu_destroy,
    };
    page_mgr_register(&ops);
}
