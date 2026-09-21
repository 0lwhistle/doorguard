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

#include <stdio.h>
#include <string.h>

static int on_face_box(const event_t *e, void *ud)
{
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
    (void)e;
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

/* ---- 服务层请求 UI 弹窗/改框(验证流程;UI 只渲染不决策) ---- */

static int on_ask_uid(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_ASK_UID;
    ui_evt_push(&evt);
    return 0;
}

static int on_input_pwd(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_INPUT_PWD;
    evt.input_req = *(const ev_ui_input_req_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_pick_method(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_PICK_METHOD;
    evt.methods = *(const ev_ui_methods_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_result(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_RESULT;
    evt.result_popup = *(const ev_ui_result_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_facebox(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_FACEBOX;
    evt.facebox = *(const ev_ui_facebox_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_ntp_result(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_NTP_RESULT;
    evt.ntp = *(const ev_ntp_result_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_hint_clear(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_HINT_CLEAR;
    ui_evt_push(&evt);
    return 0;
}

/* ---- web 上位机账号(设备管理 → Web 管理) ---- */

static int on_web_state(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_WEB_STATE;
    evt.web_state = *(const ev_web_state_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

static int on_web_set_result(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_WEB_SET_RESULT;
    evt.web_set_result = *(const ev_web_set_result_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

/* 人脸质量判定 → 拍摄页实时提示(可拍/太糊/太小;主页等页面忽略) */
static int on_vision_quality(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_QUALITY;
    evt.quality = *(const ev_vision_quality_t *)e->data;
    ui_evt_push(&evt);
    return 0;
}

/* 录入结果(人脸录入/清除/删除)→ 用户编辑页刷新与弹窗 */
static int on_enroll_result(const event_t *e, void *ud)
{
    (void)ud;
    ui_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = UI_EVT_ENROLL_RESULT;
    evt.enroll = *(const ev_enroll_result_t *)e->data;
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
    event_bus_subscribe(EV_UI_ASK_UID, on_ask_uid, NULL);
    event_bus_subscribe(EV_UI_INPUT_PWD, on_input_pwd, NULL);
    event_bus_subscribe(EV_UI_PICK_METHOD, on_pick_method, NULL);
    event_bus_subscribe(EV_UI_RESULT, on_result, NULL);
    event_bus_subscribe(EV_UI_HINT_CLEAR, on_hint_clear, NULL);
    event_bus_subscribe(EV_UI_FACEBOX, on_facebox, NULL);
    event_bus_subscribe(EV_NET_NTP_RESULT, on_ntp_result, NULL);
    event_bus_subscribe(EV_NET_WEB_STATE, on_web_state, NULL);
    event_bus_subscribe(EV_NET_WEB_SET_RESULT, on_web_set_result, NULL);
    event_bus_subscribe(EV_ENROLL_RESULT, on_enroll_result, NULL);
    event_bus_subscribe(EV_VISION_QUALITY, on_vision_quality, NULL);
    DG_LOGI("[BRIDGE]", "事件桥就绪(15 订阅)");
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

void bridge_uid_submit(const char *uid)
{
    ev_text_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_UID;
    snprintf(in.text, sizeof(in.text), "%s", uid ? uid : "");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
}

void bridge_pwd_submit(const char *uid, const char *pwd)
{
    ev_text_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_PWD;
    snprintf(in.uid, sizeof(in.uid), "%s", uid ? uid : "");
    snprintf(in.text, sizeof(in.text), "%s", pwd ? pwd : "");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
}

void bridge_method_pick(int32_t method)
{
    ev_method_pick_t mp = { .method = method };
    EVENT_BUS_PUBLISH(EV_UI_METHOD_PICK, &mp);
}

void bridge_cancel(void)
{
    ev_ui_btn_t b = { .btn = DG_BTN_BACK };
    EVENT_BUS_PUBLISH(EV_UI_BTN, &b);
}

/* ---- web 上位机账号:动作经 net 模块,UI 不碰凭据存储 ---- */

void bridge_web_state_req(void)
{
    EVENT_BUS_PUBLISH_EMPTY(EV_NET_WEB_STATE_REQ);
}

void bridge_web_set(const char *user, const char *pwd)
{
    ev_web_set_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.user, sizeof(req.user), "%s", user ? user : "");
    snprintf(req.pwd, sizeof(req.pwd), "%s", pwd ? pwd : "");
    EVENT_BUS_PUBLISH(EV_NET_WEB_SET, &req);
}
