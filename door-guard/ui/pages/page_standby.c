/*
 * page_standby.c — 待机页(spec-ui §3.2):全黑 + 大字挂钟(150px 专属数字
 * 字体)+ 日期行,每秒刷新;触摸/人脸唤醒(TOUCH 事件经 FSM;本页点击也喂
 * FSM_EV_TOUCH)
 *
 * 背光编排(M1/M2 功耗管理,2026-10-04):本页 create/destroy 就是「进/出
 * 待机」的准确边界(唤醒 = FSM → EV_UI_GOTO_PAGE → navigator 切页必经
 * destroy),亮度策略收敛在此,不另设服务:
 *   create → 降到 min(用户设置, 30%)(用户已调暗时不得反向变亮)
 *   create → 10s 一次性定时器 → 全黑(亮度 0)+ 暂停时钟刷新(黑屏重绘纯浪费)
 *   destroy(任何唤醒切页)→ 删定时器 + 恢复用户设置亮度
 * 全黑状态下触摸仍有效:evdev 采样不经过背光,按下沿照常 bridge_touch。
 */
#include "cfg.h"
#include "modules/display/display.h"
#include "dg_log.h"
#include "events.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "theme.h"

#include <stdio.h>
#include <time.h>

/* 2026-10-04 规格定值:进待机降至最大亮度的 30%,再 10s 无唤醒全黑 */
#define STANDBY_BRIGHTNESS_PCT 30
#define STANDBY_BLACKOUT_MS    10000

static lv_obj_t *s_clock = NULL;
static lv_obj_t *s_date = NULL;
static lv_timer_t *s_timer = NULL;
static lv_timer_t *s_blackout = NULL;

static void on_touch(lv_event_t *e)
{
    (void)e;
    DG_LOGI("[STANDBY]", "touch wake");
    bridge_touch();      /* 触摸唤醒由 access 服务统一决策(EV_UI_GOTO_PAGE 回 UI) */
}

/* 周几文案(键=原文「周日」…「周六」;日期本体走纯数字,无语言差异)。
 * 每次调用重建:静态数组会把首次的译文指针钉死,吃不到语言热切换 */
static const char *weekday_key(int wday)
{
    const char *keys[7] = {
        _("周日"), _("周一"), _("周二"), _("周三"), _("周四"), _("周五"),
        _("周六"),
    };
    return keys[wday % 7];
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
    if (s_date) {
        /* 日期本体纯数字(无语言差异,也避开裸中文扫描);周几走 i18n 键 */
        char db[40];
        snprintf(db, sizeof(db), "%02d-%02d %s", tmv.tm_mon + 1, tmv.tm_mday,
                 weekday_key(tmv.tm_wday));
        lv_label_set_text(s_date, db);
    }
}

/* 10s 无唤醒:背光全灭(屏已无可见内容,时钟刷新一并暂停);
 * 唤醒不经过本页——GOTO_PAGE 切页时 destroy 恢复,定时器随页删除 */
static void blackout_timer_cb(lv_timer_t *t)
{
    (void)t;
    (void)display_backlight_set(0);
    if (s_timer)
        lv_timer_pause(s_timer);
    DG_LOGI("[STANDBY]", "10s 无唤醒,背光全黑");
}

void page_standby_create(lv_obj_t *parent)
{
    DG_LOGI("[STANDBY]", "page create");
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 大字挂钟:150px 专属数字字体(DejaVu Bold 0-9+冒号),黑底纯白;
     * 视觉重心略上移,下方 30px 次级日期行(65% 白)留出呼吸感 */
    s_clock = lv_label_create(parent);
    lv_obj_set_style_text_color(s_clock, DG_COL_BG(), 0);
    lv_obj_set_style_text_font(s_clock, DG_FONT_CLOCK, 0);
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -40);

    s_date = lv_label_create(parent);
    lv_obj_set_style_text_color(s_date, DG_COL_BG(), 0);
    lv_obj_set_style_text_opa(s_date, LV_OPA_70, 0);
    lv_obj_set_style_text_font(s_date, DG_FONT_CN, 0);
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 72);
    lv_obj_clear_flag(s_date, LV_OBJ_FLAG_CLICKABLE);

    /* 两个标签都就位后再首刷:label 默认文案是「Text」,先建后刷会让日期
     * 位置闪现一秒默认文案再被真日期顶掉(2026-10-04 板上实测) */
    clock_timer_cb(NULL);

    s_timer = lv_timer_create(clock_timer_cb, 1000, NULL);

    /* 待机降亮:min(用户设置, 30%) */
    const int cur = cfg_get()->brightness;
    (void)display_backlight_set(cur < STANDBY_BRIGHTNESS_PCT
                                    ? cur : STANDBY_BRIGHTNESS_PCT);
    s_blackout = lv_timer_create(blackout_timer_cb, STANDBY_BLACKOUT_MS, NULL);

    /* 整页点击 = 触摸唤醒。两处标志缺一不可(实测):
     * - 容器默认 SCROLLABLE,手指稍动即被判为滚动手势,CLICKED 永不触发;
     * - 用 PRESSED 而非 CLICKED:按下即唤醒,无抬起确认延迟;
     * - 时钟 label 置为不可点,避免中央时钟 label 拦截父对象事件形成死区 */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);
}

void page_standby_destroy(void)
{
    if (s_blackout) {
        lv_timer_delete(s_blackout);
        s_blackout = NULL;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_clock = NULL;
    s_date = NULL;
    /* 唤醒即恢复用户设置亮度(全黑后本函数是唯一恢复路径) */
    (void)display_backlight_set(cfg_get()->brightness);
    DG_LOGI("[STANDBY]", "page destroy");
}
