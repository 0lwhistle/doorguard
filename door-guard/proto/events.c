/*
 * events.c — EV_* 事件名表
 */
#include "events.h"
#include "event_bus.h"

#include <string.h>

static const struct {
    event_type_t type;
    const char *name;
} s_ev_names[] = {
    { EV_AUTH_RESULT,      "AUTH_RESULT" },
    { EV_AUTH_DOOR_OPEN,   "AUTH_DOOR_OPEN" },
    { EV_AUTH_DOOR_CLOSE,  "AUTH_DOOR_CLOSE" },
    { EV_VISION_FACE_BOX,  "VISION_FACE_BOX" },
    { EV_VISION_FACE_LOST, "VISION_FACE_LOST" },
    { EV_VISION_MATCH_1N,  "VISION_MATCH_1N" },
    { EV_VISION_VERIFY_11, "VISION_VERIFY_11" },
    { EV_VISION_SET_MODE,  "VISION_SET_MODE" },
    { EV_VISION_CAPTURE_REQ, "VISION_CAPTURE_REQ" },
    { EV_VISION_FEATURE,   "VISION_FEATURE" },
    { EV_CAPTURE_STATE,    "CAPTURE_STATE" },
    { EV_ENROLL_REQUEST,   "ENROLL_REQUEST" },
    { EV_ENROLL_PROGRESS,  "ENROLL_PROGRESS" },
    { EV_ENROLL_RESULT,    "ENROLL_RESULT" },
    { EV_NET_STATE,        "NET_STATE" },
    { EV_NET_NTP_RESULT,   "NET_NTP_RESULT" },
    { EV_NET_OTA_PROGRESS, "NET_OTA_PROGRESS" },
    { EV_FINGER_STATUS,    "FINGER_STATUS" },
    { EV_IC_CARD,          "IC_CARD" },
    { EV_DOOR_STATE,       "DOOR_STATE" },
    { EV_UI_STANDBY,       "UI_STANDBY" },
    { EV_UI_ASK_UID,       "UI_ASK_UID" },
    { EV_UI_INPUT_PWD,     "UI_INPUT_PWD" },
    { EV_UI_PICK_METHOD,   "UI_PICK_METHOD" },
    { EV_UI_RESULT,        "UI_RESULT" },
    { EV_UI_HINT_CLEAR,    "UI_HINT_CLEAR" },
    { EV_UI_FACEBOX,       "UI_FACEBOX" },
    { 0, NULL },
};

const char *dg_event_name(event_type_t type)
{
    for (int i = 0; s_ev_names[i].name != NULL; i++) {
        if (s_ev_names[i].type == type)
            return s_ev_names[i].name;
    }
    return event_bus_get_type_name(type);
}
