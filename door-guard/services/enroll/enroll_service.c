/*
 * enroll_service.c — 录入编排实现
 *
 * 人脸特征入库前查重(spec-database §2:相似度语义)由 storage 的
 * 特征比较器执行——本服务负责"取特征→入库→回执"的编排;1:N 查重的
 * 真算法比较器在 vision 后端就绪后经 storage_set_feature_cmp 注入。
 */
#include "enroll_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "vision_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[ENROLL]";

static event_subscription_t *s_subs[2];
static int s_sub_cnt = 0;

static void publish_result(const char *uid, int32_t kind, uint32_t seq, int err)
{
    ev_enroll_result_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.seq = seq;
    ev.err = err;
    EVENT_BUS_PUBLISH(EV_ENROLL_RESULT, &ev);
}

/* 视觉特征回执:取槽位 → 查重入库 */
static int on_feature(const event_t *e, void *ud)
{
    (void)ud;
    const ev_feature_t *f = (const ev_feature_t *)e->data;

    uint8_t plain[DG_FEATURE_MAX];
    size_t len = 0;
    if (vision_service_fetch_feature(f->seq, plain, sizeof(plain), &len) != DG_OK) {
        DG_LOGE(TAG, "特征槽位 %u 已失效", f->seq);
        publish_result(f->user_id, DG_ENROLL_FACE, f->seq, DG_ERR_INTERNAL);
        return 0;
    }

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", f->user_id);
    memcpy(rec.face_vec, plain, len);
    rec.face_vec_len = (uint16_t)len;

    /* update 为覆盖语义:先取现有记录回填 role/auth_flags,避免清零
     * (db_user_update 契约见 storage.h) */
    user_rec_t existing;
    if (db_user_get(f->user_id, &existing) == DG_OK) {
        rec.role = existing.role;
        rec.auth_flags = existing.auth_flags;
    }

    /* 编排语义:先在用户管理页建用户(含密码),再录特征;未建则报"用户不存在" */
    int rc = db_user_update(&rec);

    if (rc == DG_OK)
        vision_service_library_add(f->user_id, plain, len);  /* 特征库 INSERT */

    memset(plain, 0, sizeof(plain));            /* 明文用后擦除 */
    publish_result(f->user_id, DG_ENROLL_FACE, f->seq, rc);
    return 0;
}

static int on_request(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_request_t *r = (const ev_enroll_request_t *)e->data;

    if (r->kind == DG_ENROLL_DELETE) {
        int rc = db_user_del(r->user_id);
        vision_service_library_remove(r->user_id);   /* 特征库 DELETE */
        publish_result(r->user_id, r->kind, r->seq, rc);
        return 0;
    }
    if (r->kind == DG_ENROLL_FACE_CLEAR) {
        /* 清除人脸(保留用户):专用接口——db_user_update 的 len=0=保留语义
         * 表达不了清除(2026-09-21 测试抓出);特征库经事件同步删除 */
        int rc = db_user_clear_face(r->user_id);
        if (rc == DG_OK)
            vision_service_library_remove(r->user_id);
        publish_result(r->user_id, r->kind, r->seq, rc);
        return 0;
    }
    if (r->kind == DG_ENROLL_FACE) {
        /* 向视觉服务请求抓取特征(板上走 ROCKIVA,sim 走 mock) */
        ev_capture_req_t req;
        memset(&req, 0, sizeof(req));
        snprintf(req.user_id, sizeof(req.user_id), "%s", r->user_id);
        req.seq = r->seq;
        EVENT_BUS_PUBLISH(EV_VISION_CAPTURE_REQ, &req);
        return 0;
    }
    /* 指纹录入:Phase 8 uart_hal 接入后实现 */
    publish_result(r->user_id, r->kind, r->seq, DG_ERR_NOT_INIT);
    return 0;
}

int enroll_service_start(void)
{
    if (s_sub_cnt)
        return DG_OK;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_ENROLL_REQUEST, on_request, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_FEATURE, on_feature, NULL);
    DG_LOGI(TAG, "enroll 服务启动");
    return DG_OK;
}

void enroll_service_stop(void)
{
    for (int i = 0; i < s_sub_cnt; i++) {
        event_bus_unsubscribe(s_subs[i]);
        s_subs[i] = NULL;
    }
    s_sub_cnt = 0;
}
