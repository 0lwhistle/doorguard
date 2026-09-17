/*
 * ui_events.h — UI 事件队列(LVGL 线程安全桥)
 *
 * 纪律:LVGL 非线程安全。event_bus 分发线程的回调里**禁止直接调 LVGL**;
 * 总线回调只入队(ui_evt_push),LVGL 线程的泵定时器(100ms)出队渲染。
 */
#ifndef DG_UI_EVENTS_H
#define DG_UI_EVENTS_H

#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_EVT_FACE_BOX = 0,
    UI_EVT_FACE_LOST,
    UI_EVT_AUTH_RESULT,
    UI_EVT_HINT,
    UI_EVT_GOTO_PAGE,
} ui_evt_kind_t;

typedef struct {
    ui_evt_kind_t kind;
    ev_face_box_t box;
    ev_auth_result_t result;
    ev_hint_t hint;
    ev_goto_page_t page;
} ui_evt_t;

/** 入队(任意线程;队满丢弃并计数) */
void ui_evt_push(const ui_evt_t *e);

/** 出队一条(LVGL 线程);空返回 false */
int ui_evt_pop(ui_evt_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DG_UI_EVENTS_H */
