/*
 * vision_service.c — 视觉服务实现
 *
 * 槽位:8 个特征槽(环形),事件 EV_VISION_FEATURE 携 (user_id, seq, len),
 * enroll 按 seq 经 vision_service_fetch_feature 取明文(用后擦除)。
 * 工作模式:EV_VISION_SET_MODE(access 1s tick 联动)写入,后端每帧/每回调读取。
 * 后端:注册表 + 契约见 vision_backend.h;本文件不认识任何具体模型/库。
 */
#include "vision_service.h"
#include "vision_backend.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- 后端注册表(契约见 vision_backend.h;装配层注册,服务层只认 ops) ---- */

#define BACKEND_MAX 4
static const vision_backend_ops_t *s_backends[BACKEND_MAX];
static int s_backend_cnt;
static const vision_backend_ops_t *s_active;
static bool s_features_compatible = true;   /* 口径校验结果(后端发布命中前查) */

int vision_backend_register(const vision_backend_ops_t *ops)
{
    if (!ops || !ops->name || !ops->name[0] || !ops->start)
        return DG_ERR_PARAM;
    for (int i = 0; i < s_backend_cnt; i++) {
        if (!strcmp(s_backends[i]->name, ops->name)) {
            s_backends[i] = ops;                /* 同名覆盖(重编/测试友好) */
            return DG_OK;
        }
    }
    if (s_backend_cnt >= BACKEND_MAX)
        return DG_ERR_NO_MEMORY;
    s_backends[s_backend_cnt++] = ops;
    return DG_OK;
}

const vision_backend_ops_t *vision_backend_active(void)
{
    return s_active;
}

const char *vision_backend_name(void)
{
    return (s_active && s_active->name) ? s_active->name : "-";
}

/* 生效的特征口径:cfg face.model_tag 优先(同一框架换 .data 文件时,编译器
 * 无从知道,只能靠配置声明),否则用后端自带的框架默认口径 */
static const char *effective_model_tag(const vision_backend_ops_t *ops)
{
    const dg_cfg_t *c = cfg_get();
    if (c->face_model_tag[0])
        return c->face_model_tag;
    return (ops && ops->model_tag && ops->model_tag[0]) ? ops->model_tag : NULL;
}

const char *vision_backend_model_tag(void)
{
    const char *tag = effective_model_tag(s_active);
    return tag ? tag : "-";
}

bool vision_service_features_compatible(void)
{
    return s_features_compatible;
}

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

const char *vision_mode_name(dg_vision_mode_t m){
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
    if (s_active && s_active->on_mode)
        s_active->on_mode(mode, uid);
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

/* ---- 后端选择与启动(契约见 vision_backend.h) ---- */

static const char *env_or(const char *k, const char *dflt)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : dflt;
}

/* 挑选后端:env DG_VISION_BACKEND > cfg face.backend > 第一个注册的 */
static const vision_backend_ops_t *pick_backend(void)
{
    const char *want = env_or("DG_VISION_BACKEND", NULL);
    if (!want || !want[0])
        want = cfg_get()->face_backend;         /* "" = 不指定 */
    if (want && want[0]) {
        for (int i = 0; i < s_backend_cnt; i++) {
            if (!strcmp(s_backends[i]->name, want))
                return s_backends[i];
        }
        DG_LOGW(TAG, "指定后端 '%s' 未注册,回退默认", want);
    }
    return s_backend_cnt > 0 ? s_backends[0] : NULL;
}

/* 特征口径校验(换模型 = 特征空间变了,旧特征不可比):
 * 生效口径(见 effective_model_tag)与 device_config 里登记的值比对——
 * 首次启动写入;不一致则 features_compatible=false(后端据此不发命中:
 * 宁可不开门,不可错开门)。换模型的正确流程见 modules/vision/README.md。 */
static void check_model_tag(const vision_backend_ops_t *ops)
{
    const char *tag = effective_model_tag(ops);
    if (!tag)
        return;                                 /* 未声明口径(PC mock 等):不校验 */

    char cur[64] = { 0 };
    if (db_config_get("face_model_tag", cur, sizeof(cur)) != DG_OK || !cur[0]) {
        if (db_config_set("face_model_tag", tag) == DG_OK)
            DG_LOGI(TAG, "特征口径登记 face_model_tag=%s", tag);
        return;
    }
    if (!strcmp(cur, tag))
        return;

    s_features_compatible = false;
    DG_LOGE(TAG, "人脸特征口径不一致:库=%s 当前=%s —— 模型换过,旧特征无法比对,"
                 "1:N/1:1 命中已屏蔽;请重新录入全部人脸,再把 device_config 表的 "
                 "face_model_tag 改为 '%s' 后重启", cur, tag, tag);
}

int vision_backend_start(bool enable_mock)
{
    const vision_backend_ops_t *ops = pick_backend();
    if (!ops) {
        DG_LOGE(TAG, "没有任何视觉后端注册(main.c 装配遗漏)");
        return DG_ERR_NOT_INIT;
    }
    s_active = ops;

    /* 服务层接线:后端只提供能力,注入点集中在这里(换后端不必改这些) */
    vision_service_set_lib_ops(ops->lib_add, ops->lib_del);
    if (ops->compare)
        storage_set_feature_cmp(ops->compare, NULL, NULL);
    check_model_tag(ops);
    if (cfg_get()->liveness_enable && !ops->has_landmarks)
        DG_LOGW(TAG, "liveness_enable=1 但后端 '%s' 不提供关键点:活体无法生效",
                ops->name);

    int rc = ops->start(enable_mock);
    DG_LOGI(TAG, "后端 %s(model_tag=%s,关键点=%s)启动%s", ops->name,
            vision_backend_model_tag(), ops->has_landmarks ? "有" : "无",
            rc == DG_OK ? "成功" : "失败(降级:无检测/无识别)");
    return rc;
}

int vision_service_start(void)
{
    if (s_running)
        return DG_OK;
    /* 模式下发订阅(bus 分发线程执行;后端在此之后读取模式) */
    s_sub = event_bus_subscribe(EV_VISION_SET_MODE, on_set_mode, NULL);
    s_running = true;
    DG_LOGI(TAG, "vision 服务启动(后端由 CMake 编入 + holder 装配注册)");
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
