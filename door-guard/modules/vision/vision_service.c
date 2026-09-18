/*
 * vision_service.c — 视觉服务实现
 *
 * 槽位:8 个特征槽(环形),事件 EV_VISION_FEATURE 携 (user_id, seq, len),
 * enroll 按 seq 经 vision_service_fetch_feature 取明文(用后擦除)。
 */
#include "vision_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "[VISION]";

#define FEATURE_SLOTS 8

typedef struct {
    bool used;
    uint32_t seq;
    char user_id[DG_UID_LEN];
    uint8_t data[DG_FEATURE_MAX];
    uint16_t len;
} feature_slot_t;

static feature_slot_t s_slots[FEATURE_SLOTS];
static uint32_t s_slot_pos = 0;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_running = false;

int vision_service_submit_feature(const char *user_id, uint32_t seq,
                                  const uint8_t *feature, size_t len)
{
    if (!user_id || !feature || len == 0 || len > DG_FEATURE_MAX)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    feature_slot_t *slot = &s_slots[s_slot_pos];
    s_slot_pos = (s_slot_pos + 1) % FEATURE_SLOTS;
    snprintf(slot->user_id, sizeof(slot->user_id), "%s", user_id);
    slot->seq = seq;
    memcpy(slot->data, feature, len);
    slot->len = (uint16_t)len;
    slot->used = true;
    pthread_mutex_unlock(&s_mtx);

    /* 句柄事件:特征本体不过总线(架构:大数据走槽位/环形缓冲) */
    ev_feature_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", user_id);
    ev.seq = seq;
    ev.len = (uint16_t)len;
    EVENT_BUS_PUBLISH(EV_VISION_FEATURE, &ev);
    return DG_OK;
}

int vision_service_fetch_feature(uint32_t seq, uint8_t *out, size_t cap, size_t *len)
{
    if (!out || !len)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < FEATURE_SLOTS; i++) {
        if (s_slots[i].used && s_slots[i].seq == seq) {
            if (cap < s_slots[i].len) {
                pthread_mutex_unlock(&s_mtx);
                return DG_ERR_PARAM;
            }
            memcpy(out, s_slots[i].data, s_slots[i].len);
            *len = s_slots[i].len;
            memset(s_slots[i].data, 0, sizeof(s_slots[i].data));  /* 明文即取即清 */
            s_slots[i].used = false;
            pthread_mutex_unlock(&s_mtx);
            return DG_OK;
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return DG_ERR_NOT_FOUND;
}

static vision_lib_add_fn s_lib_add;
static vision_lib_del_fn s_lib_del;

void vision_service_set_lib_ops(vision_lib_add_fn add, vision_lib_del_fn del)
{
    s_lib_add = add;
    s_lib_del = del;
}

int vision_service_library_add(const char *user_id, const uint8_t *feature,
                               uint16_t len)
{
    return s_lib_add ? s_lib_add(user_id, feature, len) : DG_ERR_NOT_INIT;
}

int vision_service_library_remove(const char *user_id)
{
    return s_lib_del ? s_lib_del(user_id) : DG_ERR_NOT_INIT;
}

int vision_service_start(void)
{
    s_running = true;
    DG_LOGI(TAG, "vision 服务启动(后端:mock/rockiva 由构建选择)");
    return DG_OK;
}

void vision_service_stop(void)
{
    s_running = false;
}
