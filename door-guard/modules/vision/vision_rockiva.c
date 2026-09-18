/*
 * vision_rockiva.c — 板上 ROCKIVA 视觉后端(B7,2026-09-18)
 *
 * 链路:camera NV12 帧(零拷贝包 RockIvaImage)→ ROCKIVA_PushFrame(异步)
 *   ├─ detCallback:人脸框(万分比)→ 旋转映射到竖屏坐标 → EV_VISION_FACE_BOX
 *   └─ analyseCallback:质量合格的人脸特征 →
 *        ├─ 特征库检索 SearchFeature → 命中≥阈值 → EV_VISION_MATCH_1N(走既有 FSM)
 *        └─ 缓存最新特征;EV_VISION_CAPTURE_REQ(录入)到达时提交近 3s 内特征
 * 库同步:启动时 db_user_iter_face 全量装载;录入成功 INSERT;删除 DELETE;
 *   检索命中后再查 DB 确认用户仍存在(自愈库/DB 不同步)。
 * 查重比较器:storage_set_feature_cmp 注入 ROCKIVA_FACE_FeatureCompare(1:1)。
 *
 * 模型:ROCKIVA_Init 的 modelPath 默认 /usr/lib(DG_IVA_MODEL_DIR 可改)。
 * 板上缺人脸模型时 Init 失败,本后端降级为"无检测"(日志 ERROR 明示),
 * 录入/识别不可用但不影响其余业务。
 *
 * 线程:camera 线程喂帧;ROCKIVA 内部线程回调——出站一律 EVENT_BUS_PUBLISH
 * (总线队列),禁止在回调里碰 LVGL。
 */
#include "vision_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "cfg.h"
#include "hal/camera/camera.h"

#include <rockiva_face_api.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[VISION]";

#define IVA_LIB_NAME   "dg_users"
#define IVA_FEATURE_MIN 16                  /* 小于视为无效特征 */
#define IVA_CAPTURE_FRESH_MS 3000           /* 录入取特征的新鲜度窗 */

static RockIvaHandle s_handle;
static bool s_ready;                        /* Init+FACE_Init 成功 */
static const char *env_or(const char *k, const char *dflt)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : dflt;
}

/* 最新特征缓存(analyse 回调写,capture 请求读) */
static struct {
    pthread_mutex_t mtx;
    uint8_t data[DG_FEATURE_MAX];
    uint16_t len;
    int64_t ms;
} s_cap = { .mtx = PTHREAD_MUTEX_INITIALIZER };

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 万分比矩形(原图 w×h)→ 像素 → 旋转 90° 后的竖屏坐标(RGA ROT_90 顺时针:
 * 显示点 = (H-1-y, x);若实测脸框镜像,换 ROT_270 公式并在 README 记录) */
static void rect_to_screen(const RockIvaRectangle *r, int src_w, int src_h,
                           int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh)
{
    int px = (int)((int64_t)r->topLeft.x * src_w / 10000);
    int py = (int)((int64_t)r->topLeft.y * src_h / 10000);
    int pw = (int)((int64_t)r->bottomRight.x * src_w / 10000) - px;
    int ph = (int)((int64_t)r->bottomRight.y * src_h / 10000) - py;
    *ox = src_h - (py + ph);
    *oy = px;
    *ow = ph;
    *oh = pw;
}

/* ---- 喂帧(camera 线程):零拷贝包 NV12,ROCKIVA 异步消费 ---- */
static void on_frame_push(const uint8_t *data, int w, int h, uint32_t frame_id)
{
    if (!s_ready)
        return;
    RockIvaImage img;
    memset(&img, 0, sizeof(img));
    img.frameId = frame_id;
    img.info.width = (uint16_t)w;
    img.info.height = (uint16_t)h;
    img.info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    img.dataAddr = (uint8_t *)data;
    img.dataFd = -1;                    /* V4L2 mmap 虚拟地址,无 DMA fd */
    ROCKIVA_PushFrame(s_handle, &img, NULL);
}

/* ---- 检测回调:脸框 → EV_VISION_FACE_BOX(150ms 节流) ---- */
static void on_det(const RockIvaFaceDetResult *result,
                   const RockIvaExecuteStatus status, void *ud)
{
    (void)ud;
    if ((int)status != ROCKIVA_RET_SUCCESS || !result || result->objNum == 0)
        return;

    static int64_t last_ms;
    int64_t now = now_ms();
    if (now - last_ms < 150)
        return;
    last_ms = now;

    /* 取最大脸(门口场景通常单脸) */
    const RockIvaFaceInfo *best = &result->faceInfo[0];
    for (uint32_t i = 1; i < result->objNum && i < ROCKIVA_FACE_MAX_FACE_NUM; i++) {
        const RockIvaFaceInfo *f = &result->faceInfo[i];
        if ((int32_t)(f->faceRect.bottomRight.x - f->faceRect.topLeft.x) *
                (f->faceRect.bottomRight.y - f->faceRect.topLeft.y) >
            (int32_t)(best->faceRect.bottomRight.x - best->faceRect.topLeft.x) *
                (best->faceRect.bottomRight.y - best->faceRect.topLeft.y))
            best = f;
    }

    ev_face_box_t box;
    memset(&box, 0, sizeof(box));
    box.state = DG_BOX_DETECTED;
    rect_to_screen(&best->faceRect, 1280, 720, &box.x, &box.y, &box.w, &box.h);
    EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
}

/* ---- 分析回调:特征 → 1:N 检索 + 录入缓存 ---- */
static void on_analyse(const RockIvaFaceCapResults *result,
                       const RockIvaExecuteStatus status, void *ud)
{
    (void)ud;
    if ((int)status != ROCKIVA_RET_SUCCESS || !result)
        return;

    for (uint32_t i = 0; i < result->num; i++) {
        const RockIvaFaceCapResult *r = &result->faceResults[i];
        if (r->qualityResult != ROCKIVA_FACE_QUALITY_OK)
            continue;
        uint16_t flen = (uint16_t)r->faceAnalyseInfo.featureSize;
        const char *feat = r->faceAnalyseInfo.feature;
        if (flen < IVA_FEATURE_MIN || flen > DG_FEATURE_MAX)
            continue;

        /* 缓存最新特征(录入取用) */
        pthread_mutex_lock(&s_cap.mtx);
        memcpy(s_cap.data, feat, flen);
        s_cap.len = flen;
        s_cap.ms = now_ms();
        pthread_mutex_unlock(&s_cap.mtx);

        /* 1:N 检索 */
        RockIvaFaceSearchResults sr;
        memset(&sr, 0, sizeof(sr));
        if (ROCKIVA_FACE_SearchFeature(IVA_LIB_NAME, feat, flen, 1, 1, &sr) !=
            ROCKIVA_RET_SUCCESS)
            continue;
        if (sr.num < 1)
            continue;
        const RockIvaFaceSearchResult *best = &sr.faceIdScore[0];
        int32_t permille = (int32_t)(best->score * 1000.0f + 0.5f);

        user_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        if (best->score < cfg_get()->face_dup_threshold ||
            db_user_get(best->faceIdInfo, &rec) != DG_OK)
            continue;                        /* 分数不足 / 库与 DB 不同步(自愈) */

        ev_match_t m;
        memset(&m, 0, sizeof(m));
        m.matched = true;
        snprintf(m.user_id, sizeof(m.user_id), "%s", rec.user_id);
        snprintf(m.user_name, sizeof(m.user_name), "%s", rec.user_name);
        m.role = rec.role;
        m.score_permille = permille;
        EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &m);
        DG_LOGI(TAG, "1:N 命中 %s(%s) %d‰", rec.user_id, rec.user_name,
                permille);
    }
}

/* ---- 帧释放:ROCKIVA 用完 → 归还 V4L2 缓冲(跨线程 QBUF,内核串行化) ---- */
static void on_frames_release(const RockIvaReleaseFrames *frames, void *ud)
{
    (void)ud;
    if (!frames)
        return;
    for (uint32_t i = 0; i < frames->count; i++)
        camera_nv12_release(frames->frames[i].frameId);
}

/* ---- 录入抓取请求:近 3s 内特征直接提交 ---- */
static int on_capture_req(const event_t *e, void *ud)
{
    (void)ud;
    const ev_capture_req_t *r = (const ev_capture_req_t *)e->data;

    pthread_mutex_lock(&s_cap.mtx);
    int64_t age = now_ms() - s_cap.ms;
    uint16_t len = s_cap.len;
    uint8_t buf[DG_FEATURE_MAX];
    if (len && age <= IVA_CAPTURE_FRESH_MS)
        memcpy(buf, s_cap.data, len);
    else
        len = 0;
    pthread_mutex_unlock(&s_cap.mtx);

    if (!len) {
        DG_LOGW(TAG, "录入取特征:近 %dms 无合格人脸(请正对镜头重试)",
                IVA_CAPTURE_FRESH_MS);
        return 0;
    }
    vision_service_submit_feature(r->user_id, r->seq, buf, len);
    return 0;
}

/* ---- 查重比较器:1=重复 0=不重复 <0=错误(storage 契约) ---- */
static int rockiva_cmp(const uint8_t *a, uint16_t alen,
                       const uint8_t *b, uint16_t blen, void *ud)
{
    (void)ud;
    if (!a || !b || alen < IVA_FEATURE_MIN || blen < IVA_FEATURE_MIN)
        return -1;
    float score = 0;
    if (ROCKIVA_FACE_FeatureCompare(a, b, &score) != ROCKIVA_RET_SUCCESS)
        return -1;
    return score >= cfg_get()->face_dup_threshold ? 1 : 0;
}

/* ---- 库同步 ---- */
static int lib_iter_cb(const char *user_id, const uint8_t *plain,
                       uint16_t len, void *ud)
{
    (void)ud;
    RockIvaFaceIdInfo id;
    memset(&id, 0, sizeof(id));
    snprintf(id.faceIdInfo, sizeof(id.faceIdInfo), "%s", user_id);
    RockIvaRetCode rc = ROCKIVA_FACE_FeatureLibraryControl(
        IVA_LIB_NAME, ROCKIVA_FACE_FEATURE_INSERT, &id, 1, plain, len);
    if (rc != ROCKIVA_RET_SUCCESS)
        DG_LOGW(TAG, "特征库装载 %s 失败(%d)", user_id, rc);
    else
        DG_LOGI(TAG, "特征库装载 %s(%u B)", user_id, len);
    return 0;
}

static int lib_add(const char *user_id, const uint8_t *feature, uint16_t len)
{
    if (!s_ready)
        return DG_ERR_NOT_INIT;
    RockIvaFaceIdInfo id;
    memset(&id, 0, sizeof(id));
    snprintf(id.faceIdInfo, sizeof(id.faceIdInfo), "%s", user_id);
    return ROCKIVA_FACE_FeatureLibraryControl(
        IVA_LIB_NAME, ROCKIVA_FACE_FEATURE_INSERT, &id, 1, feature, len) ==
               ROCKIVA_RET_SUCCESS ? DG_OK : DG_ERR_IO;
}

static int lib_del(const char *user_id)
{
    if (!s_ready)
        return DG_ERR_NOT_INIT;
    RockIvaFaceIdInfo id;
    memset(&id, 0, sizeof(id));
    snprintf(id.faceIdInfo, sizeof(id.faceIdInfo), "%s", user_id);
    return ROCKIVA_FACE_FeatureLibraryControl(
        IVA_LIB_NAME, ROCKIVA_FACE_FEATURE_DELETE, &id, 1, NULL, 0) ==
               ROCKIVA_RET_SUCCESS ? DG_OK : DG_ERR_IO;
}

void vision_backend_start(bool enable_mock)
{
    (void)enable_mock;                  /* 板上无 mock;该参数仅 PC sim 语义 */

    /* 服务接缝:库维护句柄 + 查重比较器 + 录入抓取订阅 */
    vision_service_set_lib_ops(lib_add, lib_del);
    storage_set_feature_cmp(rockiva_cmp, NULL, NULL);
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req, NULL);
    camera_set_nv12_listener(on_frame_push, NULL);   /* 归还走 on_frames_release→camera_nv12_release */

    RockIvaInitParam ip;
    memset(&ip, 0, sizeof(ip));
    ip.logLevel = ROCKIVA_LOG_WARN;
    ip.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    snprintf(ip.modelPath, sizeof(ip.modelPath), "%s",
             env_or("DG_IVA_MODEL_DIR", "/usr/lib"));
    ip.imageInfo.width = 1280;
    ip.imageInfo.height = 720;
    ip.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;

    RockIvaRetCode rc = ROCKIVA_Init(&s_handle, ROCKIVA_MODE_VIDEO, &ip, NULL);
    if (rc != ROCKIVA_RET_SUCCESS) {
        DG_LOGE(TAG, "ROCKIVA_Init 失败(%d):模型目录 %s 缺人脸模型?"
                     "从 SDK external/iva/.../model 拷板后重启", rc, ip.modelPath);
        return;
    }
    ROCKIVA_SetFrameReleaseCallback(s_handle, on_frames_release);

    RockIvaFaceTaskParams fp;
    memset(&fp, 0, sizeof(fp));
    fp.mode = ROCKIVA_FACE_MODE_NORMAL;
    fp.faceTaskType.faceCaptureEnable = 1;
    fp.faceTaskType.faceRecognizeEnable = 1;
    fp.faceTaskType.faceLandmarkEnable = 1;
    fp.faceCaptureRule.detectScore = 60;
    fp.faceCaptureRule.optType = ROCKIVA_FACE_OPT_FAST;
    fp.faceCaptureRule.faceQualityThrehold = 60;

    RockIvaFaceCallback cbs = { .detCallback = on_det,
                                .analyseCallback = on_analyse,
                                .postureCallback = NULL };
    rc = ROCKIVA_FACE_Init(s_handle, &fp, cbs);
    if (rc != ROCKIVA_RET_SUCCESS) {
        DG_LOGE(TAG, "ROCKIVA_FACE_Init 失败(%d)", rc);
        ROCKIVA_Release(s_handle);
        return;
    }

    db_user_iter_face(lib_iter_cb, NULL);   /* 全量装载 DB 人脸特征 */

    s_ready = true;
    DG_LOGI(TAG, "ROCKIVA 就绪(model=%s,检索阈值 %.2f)", ip.modelPath,
            cfg_get()->face_dup_threshold);
}
