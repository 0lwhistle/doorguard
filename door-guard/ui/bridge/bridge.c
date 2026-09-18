/*
 * bridge.c — 桥接层实现
 *
 * 职责一(事件入站):订阅 event_bus 后端事件,搬运进 ui_events 队列。
 *   纪律:总线回调运行在总线线程,**禁止直接调 LVGL**(实测堆损坏)——
 *   只入队,由 ui.c 的全局泵在 LVGL 线程取出分发。
 * 职责二(动作出站):页面请求 → event_bus,服务层统一决策后经
 *   EV_UI_GOTO_PAGE / EV_UI_HINT 回流。
 */
#include "bridge.h"
#include "event_bus.h"
#include "dg_log.h"

#include <string.h>

static int on_face_box(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_FACE_BOX;
    evt.box = *(const ev_face_box_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_face_lost(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_FACE_LOST;
    ui_evt_push(&evt);
    return 0;
}

static int on_auth_result(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_AUTH_RESULT;
    evt.result = *(const ev_auth_result_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_hint(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_HINT;
    evt.hint = *(const ev_hint_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_goto_page(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_GOTO_PAGE;
    evt.page = *(const ev_goto_page_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

void bridge_init(void)
{
    event_bus_subscribe(EV_VISION_FACE_BOX, on_face_box, NULL);
    event_bus_subscribe(EV_VISION_FACE_LOST, on_face_lost, NULL);
    event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    event_bus_subscribe(EV_UI_HINT, on_hint, NULL);
    event_bus_subscribe(EV_UI_GOTO_PAGE, on_goto_page, NULL);
    DG_LOGI("[BRIDGE]", "事件桥就绪(5 订阅)");
}

void bridge_btn(const ev_ui_btn_t *btn)
{
    if (btn)
        EVENT_BUS_PUBLISH(EV_UI_BTN, btn);
}

void bridge_touch(void)
{
    EVENT_BUS_PUBLISH_EMPTY(EV_UI_TOUCH);
}
