/*
 * vision_service.c — 视觉服务实现
 *
 * 槽位:8 个特征槽(环形),事件 EV_VISION_FEATURE 携 (user_id, seq, len),
 * enroll 按 seq 经 vision_service_fetch_feature 取明文(用后擦除)。
 * 工作模式:EV_VISION_SET_MODE(access 1s tick 联动)写入,后端每帧/每回调读取。
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
static event_subscription_t *s_sub;

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

/* ---- 工作模式 ---- */

static struct {
    pthread_mutex_t mtx;
    dg_vision_mode_t mode;
    char uid[DG_UID_LEN];
} s_mode = {
    /* 开机默认 1:N:与 FSM 初始态(ST_NORMAL + match_enabled)一致,
     * 首个 access tick(≤1s)会按 FSM 真实状态纠正 */
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .mode = DG_VMODE_DETECT_1N,
};

static vision_mode_hook_fn s_mode_hook;

const char *vision_mode_name(dg_vision_mode_t m)
{
    switch (m) {
    case DG_VMODE_IDLE:        return "IDLE";
    case DG_VMODE_DETECT_ONLY: return "DETECT_ONLY";
    case DG_VMODE_DETECT_1N:   return "DETECT_1N";
    case DG_VMODE_VERIFY_11:   return "VERIFY_11";
    default:                   return "?";
    }
}

int vision_service_set_mode(dg_vision_mode_t mode, const char *user_id)
{
    if (mode < 0 || mode >= DG_VMODE_MAX)
        return DG_ERR_PARAM;

    bool changed;
    char uid[DG_UID_LEN];
    uid[0] = '\0';
    if (mode == DG_VMODE_VERIFY_11 && user_id && user_id[0])
        snprintf(uid, sizeof(uid), "%s", user_id);

    pthread_mutex_lock(&s_mode.mtx);
    changed = (s_mode.mode != mode) || strcmp(s_mode.uid, uid) != 0;
    s_mode.mode = mode;
    snprintf(s_mode.uid, sizeof(s_mode.uid), "%s", uid);
    pthread_mutex_unlock(&s_mode.mtx);

    /* 钩子在锁外调用(后端会查 DB,不能持锁) */
    if (!changed)
        return DG_OK;
    if (s_mode_hook)
        s_mode_hook(mode, uid);
    DG_LOGI(TAG, "工作模式 → %s%s%s", vision_mode_name(mode),
            uid[0] ? " 目标 " : "", uid);
    return DG_OK;
}

dg_vision_mode_t vision_service_get_mode(void)
{
    dg_vision_mode_t m;
    pthread_mutex_lock(&s_mode.mtx);
    m = s_mode.mode;
    pthread_mutex_unlock(&s_mode.mtx);
    return m;
}

void vision_service_get_verify_uid(char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    pthread_mutex_lock(&s_mode.mtx);
    snprintf(out, cap, "%s", s_mode.mode == DG_VMODE_VERIFY_11 ? s_mode.uid : "");
    pthread_mutex_unlock(&s_mode.mtx);
}

void vision_service_set_mode_hook(vision_mode_hook_fn fn)
{
    s_mode_hook = fn;
}

/* 总线侧下发(access_service 1s tick 按 FSM 状态派生) */
static int on_set_mode(const event_t *e, void *ud)
{
    (void)ud;
    const ev_vision_mode_t *m = (const ev_vision_mode_t *)e->data;
    if (vision_service_set_mode((dg_vision_mode_t)m->mode, m->user_id) != DG_OK)
        DG_LOGW(TAG, "模式下发非法(mode=%d)", m->mode);
    return 0;
}

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
    if (s_running)
        return DG_OK;
    /* 模式下发订阅(bus 分发线程执行;后端在此之后读取模式) */
    s_sub = event_bus_subscribe(EV_VISION_SET_MODE, on_set_mode, NULL);
    s_running = true;
    DG_LOGI(TAG, "vision 服务启动(后端:mock/rockiva 由构建选择)");
    return DG_OK;
}

void vision_service_stop(void)
{
    if (s_sub) {
        event_bus_unsubscribe(s_sub);
        s_sub = NULL;
    }
    s_running = false;
}
