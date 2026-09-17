/*
 * page_standby.c — 待机页(spec-ui §3.2):全黑 + 中央 HH:MM 每秒刷新;
 * 触摸/人脸唤醒(TOUCH 事件经 FSM;本页点击也喂 FSM_EV_TOUCH)
 */
#include "auth_fsm.h"
#include "dg_log.h"
#include "page_mgr.h"
#include "theme.h"

#include <stdio.h>
#include <time.h>

static lv_obj_t *s_clock = NULL;
static lv_timer_t *s_timer = NULL;

static void on_touch(lv_event_t *e)
{
    (void)e;
    /* 触摸唤醒由 FSM 统一处理(GOTO_PAGE home) */
    extern auth_fsm_t *page_home_fsm(void);
    auth_fsm_handle(page_home_fsm(), FSM_EV_TOUCH, NULL);
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_clock)
        return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(s_clock, buf);
}

void page_standby_create(lv_obj_t *parent)
{
    DG_LOGI("[STANDBY]", "page create");
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    s_clock = lv_label_create(parent);
    lv_obj_set_style_text_color(s_clock, DG_COL_BG(), 0);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_48, 0);
    lv_obj_center(s_clock);
    clock_timer_cb(NULL);

    s_timer = lv_timer_create(clock_timer_cb, 1000, NULL);

    /* 整页点击 = 触摸唤醒 */
    lv_obj_add_event_cb(parent, on_touch, LV_EVENT_CLICKED, NULL);
}

void page_standby_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_clock = NULL;
    DG_LOGI("[STANDBY]", "page destroy");
}

void page_standby_register(void)
{
    static const dg_page_ops_t ops = {
        .name = "standby",
        .create = page_standby_create,
        .destroy = page_standby_destroy,
    };
    page_mgr_register(&ops);
}
