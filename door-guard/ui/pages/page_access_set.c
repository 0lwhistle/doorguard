/*
 * page_access_set.c — 门禁设置(spec-ui §3.3):待机超时/开门时长/密码连错锁定
 * 全部落 device_config(经 cfg_set),非法值由 cfg 层拒绝并提示。
 */
#include "cfg.h"
#include "dg_log.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdio.h>

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

static void refresh_labels(void);

/* 各项的候选值(choice 语义比自由输入更防呆) */
static const int standby_opts[] = { 15, 30, 45, 60 };
static const int door_opts[] = { 1000, 2000, 3000, 5000, 10000 };
static const int lockn_opts[] = { 3, 5, 8, 10 };
static const int locks_opts[] = { 30, 60, 120, 300 };

#define MAKE_PICKER(name, KEY, OPTS, LABEL)                              \
    static void name##_pick(void *ud, int idx)                           \
    {                                                                    \
        (void)ud;                                                        \
        int v = OPTS[idx];                                               \
        if (cfg_set_int(KEY, v) == DG_OK)                                \
            dg_popup_success(_("验证成功"), 600, NULL, NULL);            \
        else                                                             \
            dg_popup_fail(_("验证失败"), 1000, NULL, NULL);              \
    }                                                                    \
    static void name##_click(lv_event_t *e)                              \
    {                                                                    \
        (void)e;                                                         \
        static char buf[4][16];                                          \
        const char *opts[4];                                             \
        for (int i = 0; i < 4; i++) {                                    \
            snprintf(buf[i], sizeof(buf[i]), "%d", OPTS[i]);             \
            opts[i] = buf[i];                                            \
        }                                                                \
        dg_popup_choice(LABEL, opts, 4, name##_pick, NULL, NULL);        \
    }

MAKE_PICKER(standby, "standby_timeout_s", standby_opts, _("待机超时"))
MAKE_PICKER(door, "door_open_ms", door_opts, _("开门时长"))
MAKE_PICKER(lockn, "pwd_fail_lock_n", lockn_opts, _("密码连错锁定"))
MAKE_PICKER(locks, "pwd_fail_lock_s", locks_opts, _("锁定秒数"))

static lv_obj_t *s_labels[4];

static void refresh_labels(void)
{
    const dg_cfg_t *c = cfg_get();
    char t[64];
    if (s_labels[0]) {
        snprintf(t, sizeof(t), "%s: %ds", _("待机超时"), c->standby_timeout_s);
        lv_label_set_text(s_labels[0], t);
    }
    if (s_labels[1]) {
        snprintf(t, sizeof(t), "%s: %dms", _("开门时长"), c->door_open_ms);
        lv_label_set_text(s_labels[1], t);
    }
    if (s_labels[2]) {
        snprintf(t, sizeof(t), "%s: %d", _("密码连错锁定"), c->pwd_fail_lock_n);
        lv_label_set_text(s_labels[2], t);
    }
    if (s_labels[3]) {
        snprintf(t, sizeof(t), "%s: %ds", _("锁定秒数"), c->pwd_fail_lock_s);
        lv_label_set_text(s_labels[3], t);
    }
}

void page_access_set_create(lv_obj_t *parent)
{
    DG_LOGI("[ACCESS_SET]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("门禁设置"));
    lv_obj_set_style_text_font(title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    struct {
        const char *icon;
        void (*click)(lv_event_t *);
        lv_obj_t **label;
    } rows[] = {
        { LV_SYMBOL_SETTINGS, standby_click, &s_labels[0] },
        { LV_SYMBOL_OK,       door_click,    &s_labels[1] },
        { LV_SYMBOL_CLOSE,    lockn_click,   &s_labels[2] },
        { LV_SYMBOL_EYE_OPEN, locks_click,   &s_labels[3] },
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = dg_btn_create(parent, rows[i].icon, "");
        lv_obj_set_size(btn, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 110 + i * (DG_BTN_H + DG_PAD));
        lv_obj_add_event_cb(btn, rows[i].click, LV_EVENT_CLICKED, NULL);
        /* 按钮内的文本 label 动态更新 */
        lv_obj_t *row = lv_obj_get_child(btn, 0);
        *rows[i].label = lv_label_create(row);
        lv_obj_set_style_text_font(*rows[i].label, DG_FONT_CN, 0);
        lv_obj_set_style_text_color(*rows[i].label, DG_COL_BG(), 0);
    }
    refresh_labels();

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 200, DG_BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -DG_PAD);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
}

void page_access_set_destroy(void)
{
    s_labels[0] = s_labels[1] = s_labels[2] = s_labels[3] = NULL;
    DG_LOGI("[ACCESS_SET]", "page destroy");
}

void page_access_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "access_set",
        .create = page_access_set_create,
        .destroy = page_access_set_destroy,
    };
    navigator_register(&ops);
}
