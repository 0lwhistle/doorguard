/*
 * page_logs.c — 记录查询(spec-ui §3.3):时间段(+可选用户)查 access_logs,
 * 列表分页:时间/ID/姓名/方式/结果
 */
#include "dg_log.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "storage.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_list.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOGS_PAGE_SIZE 8

typedef enum {
    RANGE_1H = 0,
    RANGE_TODAY,
    RANGE_3D,
    RANGE_ALL,
} range_sel_t;

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_title = NULL;
static log_query_t s_q;                  /* 当前查询(含分页) */
static int s_total_pages = 1;

static const char *method_name(int32_t m)
{
    switch (m) {
    case DG_METHOD_FACE_1N: return _("人脸");
    case DG_METHOD_FACE_11: return _("1:1人脸");
    case DG_METHOD_FINGER:  return _("指纹");
    case DG_METHOD_PWD:     return _("密码");
    default:                return _("IC卡");
    }
}

static const char *result_name(int32_t r, int32_t reason)
{
    if (r == DG_RESULT_PASS)
        return _("通过");
    if (reason == DG_REASON_BLACKLIST)
        return _("黑名单");
    if (reason == DG_REASON_STRANGER)
        return _("陌生人");
    return _("拒绝");
}

static void refresh_logs(void)
{
    if (!s_list)
        return;
    dg_list_clear(s_list);

    access_log_t rows[LOGS_PAGE_SIZE];
    log_page_t page = { .logs = rows, .max = LOGS_PAGE_SIZE };
    if (db_log_query(&s_q, &page) != DG_OK) {
        if (s_title)
            lv_label_set_text(s_title, _("记录查询"));
        return;
    }
    s_total_pages = (int)((page.total + (uint32_t)LOGS_PAGE_SIZE - 1) / (uint32_t)LOGS_PAGE_SIZE);
    if (s_total_pages < 1)
        s_total_pages = 1;

    for (uint32_t i = 0; i < page.count; i++) {
        char tsbuf[20];
        time_t ts = (time_t)rows[i].ts;
        struct tm tmv;
        localtime_r(&ts, &tmv);
        strftime(tsbuf, sizeof(tsbuf), "%m-%d %H:%M:%S", &tmv);
        char row[128];
        snprintf(row, sizeof(row), "%s %s %s %s %s",
                 tsbuf,
                 rows[i].has_user ? rows[i].user_id : "-",
                 rows[i].has_user ? rows[i].user_name : _("陌生人"),
                 method_name(rows[i].method),
                 result_name(rows[i].result, rows[i].reason));
        dg_list_add_row(s_list, NULL, row, NULL);
    }

    if (s_title) {
        char t[48];
        snprintf(t, sizeof(t), "%s %d/%d (%u)", _("记录查询"), s_q.page,
                 s_total_pages, page.total);
        lv_label_set_text(s_title, t);
    }
}

/* ---- 预置时间段(spec 要求按时间段查询;操作员选粒度) ---- */

static void range_pick(void *ud, int idx)
{
    (void)ud;
    time_t now = time(NULL);
    s_q.ts_from = 0;
    s_q.ts_to = 0;
    switch (idx) {
    case RANGE_1H:    s_q.ts_from = (int64_t)now - 3600; break;
    case RANGE_TODAY: {
        struct tm tmv;
        localtime_r(&now, &tmv);
        tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
        s_q.ts_from = (int64_t)mktime(&tmv);
        break;
    }
    case RANGE_3D:    s_q.ts_from = (int64_t)now - 3 * 86400; break;
    default:          break;                   /* 全部 */
    }
    s_q.page = 1;
    refresh_logs();
}

static void on_range(lv_event_t *e)
{
    (void)e;
    const char *const items[] = { _("最近1小时"), _("今天"), _("最近3天"), _("全部") };
    dg_popup_choice(_("时间"), items, 4, range_pick, NULL, NULL);
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    if (s_q.page > 1) {
        s_q.page--;
        refresh_logs();
    }
}

static void on_next(lv_event_t *e)
{
    (void)e;
    if ((int)s_q.page < s_total_pages) {
        s_q.page++;
        refresh_logs();
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

void page_logs_create(lv_obj_t *parent)
{
    DG_LOGI("[LOGS]", "page create");
    memset(&s_q, 0, sizeof(s_q));
    s_q.page = 1;
    s_q.page_size = LOGS_PAGE_SIZE;
    s_q.descending = true;                  /* 默认倒序(最新在上) */

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_font(s_title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_title, DG_COL_TEXT(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 20);

    s_list = dg_list_create(parent);
    lv_obj_set_size(s_list, DG_SCREEN_W - 2 * DG_PAD, 760);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *range = dg_btn_create(parent, LV_SYMBOL_LIST, _("时间"));
    lv_obj_set_size(range, 180, DG_BTN_H);
    lv_obj_align(range, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(range, on_range, LV_EVENT_CLICKED, NULL);

    lv_obj_t *prev = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("上一页"));
    lv_obj_set_size(prev, 150, DG_BTN_H);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_MID, 0, -DG_PAD);
    lv_obj_add_event_cb(prev, on_prev, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next = dg_btn_create_light(parent, LV_SYMBOL_RIGHT, _("下一页"));
    lv_obj_set_size(next, 150, DG_BTN_H);
    lv_obj_align(next, LV_ALIGN_BOTTOM_MID, 80, -DG_PAD);
    lv_obj_add_event_cb(next, on_next, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 180, DG_BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    refresh_logs();
}

void page_logs_destroy(void)
{
    s_list = NULL;
    s_title = NULL;
    DG_LOGI("[LOGS]", "page destroy");
}
