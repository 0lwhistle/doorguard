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
    UI_EVT_ASK_UID,        /**< 弹 ID 输入框(验证流程第一步) */
    UI_EVT_INPUT_PWD,      /**< 弹密码输入框(掩码;uid 回填) */
    UI_EVT_PICK_METHOD,    /**< 弹验证方式选择(auth_flags) */
    UI_EVT_RESULT,         /**< 结果弹窗(成功/失败 + 原因文案) */
    UI_EVT_HINT_CLEAR,     /**< 清提示条 */
    UI_EVT_FACEBOX,        /**< 服务侧脸框颜色(绿/红/隐藏) */
    UI_EVT_NTP_RESULT,     /**< NTP 校正结果(设备管理页显示) */
} ui_evt_kind_t;

typedef struct {
    ui_evt_kind_t kind;
    ev_face_box_t box;
    ev_auth_result_t result;
    ev_hint_t hint;
    ev_goto_page_t page;
    ev_ui_input_req_t input_req;   /* UI_EVT_INPUT_PWD */
    ev_ui_methods_t methods;       /* UI_EVT_PICK_METHOD */
    ev_ui_result_t result_popup;   /* UI_EVT_RESULT */
    ev_ui_facebox_t facebox;       /* UI_EVT_FACEBOX */
    ev_ntp_result_t ntp;           /* UI_EVT_NTP_RESULT */
} ui_evt_t;

/** 入队(任意线程;队满丢弃并计数) */
void ui_evt_push(const ui_evt_t *e);

/** 出队一条(LVGL 线程);空返回 false */
int ui_evt_pop(ui_evt_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DG_UI_EVENTS_H */
