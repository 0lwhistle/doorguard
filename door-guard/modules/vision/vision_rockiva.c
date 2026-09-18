/*
 * vision_rockiva.c — 板上 ROCKIVA 视觉后端(B7,2026-09-18)
 *
 * 链路:camera NV12 帧(零拷贝包 RockIvaImage)→ ROCKIVA_PushFrame(异步)
 *   ├─ detCallback:人脸框(万分比)→ 旋转映射到竖屏坐标 → EV_VISION_FACE_BOX
 *   │    + 106 点关键点回灌 liveness_service_on_face
 *   └─ analyseCallback:质量合格的人脸特征 → 录入缓存(近 3s)+ 按模式分支:
 *        DETECT_1N  → FeatureLibraryControl 检索 → 命中≥cfg face_match_threshold
 *                     → EV_VISION_MATCH_1N(走既有 FSM)
 *        VERIFY_11  → 与 mode 目标用户特征 FeatureCompare → 命中 → EV_VISION_VERIFY_11
 *        DETECT_ONLY→ 不检索(菜单/待机/验证子步;录入照常取缓存)
 *        IDLE       → 不推帧(帧在入口即归还)
 * 库同步:启动时 db_user_iter_face 全量装载;录入成功 INSERT;删除 DELETE;
 *   检索命中后再查 DB 确认用户仍存在(自愈库/DB 不同步)。
 * 查重比较器:storage_set_feature_cmp 注入 ROCKIVA_FACE_FeatureCompare(1:1)。
 *
 * 模型:ROCKIVA_Init 的 modelPath 默认 /usr/lib(DG_IVA_MODEL_DIR 可改)。
 * 板上缺人脸模型时 Init 失败,本后端返回非 0(holder 记为 ERROR,降级为
 * "无检测"),录入/识别不可用但不影响其余业务。
 *
 * 线程:camera 线程喂帧;ROCKIVA 内部线程回调——出站一律 EVENT_BUS_PUBLISH
 * (总线队列),禁止在回调里碰 LVGL。
 */
#include "vision_service.h"
#include "vision_backend.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "cfg.h"
#include "liveness_service.h"
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
#define IVA_SCORE_LOG_MS 2000               /* 检索最高分日志节流(联调调阈值用) */

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

/* VERIFY_11 目标用户特征(模式切换钩子装载;analyse 回调读) */
static struct {
    pthread_mutex_t mtx;
    uint8_t data[DG_FEATURE_MAX];
    uint16_t len;
    char uid[DG_UID_LEN];
} s_target = { .mtx = PTHREAD_MUTEX_INITIALIZER };

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
    /* 未就绪/推不了必须立即归还缓冲:否则 4 缓冲耗尽 → 相机永久断流。
     * IDLE(系统级关人脸)同样在此挡掉,不做 NPU 推理。 */
    if (!s_ready || vision_service_get_mode() == DG_VMODE_IDLE) {
        camera_nv12_release(frame_id);
        return;
    }
    RockIvaImage img;
    memset(&img, 0, sizeof(img));
    img.frameId = frame_id;
    img.info.width = (uint16_t)w;
    img.info.height = (uint16_t)h;
    img.info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    img.dataAddr = (uint8_t *)data;
    img.dataFd = -1;                    /* V4L2 mmap 虚拟地址,无 DMA fd */
    if (ROCKIVA_PushFrame(s_handle, &img, NULL) != ROCKIVA_RET_SUCCESS) {
        camera_nv12_release(frame_id);  /* 队列满丢帧,归还防饿死 */
        static int n_full;
        if (n_full++ < 3)
            DG_LOGW(TAG, "PushFrame 失败(队列满?),丢帧");
    }
}

/* 关键点回灌活体(万分比 int16 → 后端无关的 dg_face_pt_t;ROCKIVA 给 106 点) */
static void feed_liveness(const RockIvaFaceInfo *f)
{
    if (!f || f->landmarksNum == 0)
        return;
    uint32_t n = f->landmarksNum;
    if (n > DG_LANDMARK_MAX)
        n = DG_LANDMARK_MAX;
    dg_face_pt_t pts[DG_LANDMARK_MAX];
    for (uint32_t i = 0; i < n; i++) {
        pts[i].x = f->landmarks[i].x;
        pts[i].y = f->landmarks[i].y;
    }
    liveness_service_on_face(pts, n, (int32_t)f->faceQuality.score);
}

/* ---- 检测回调:脸框 → EV_VISION_FACE_BOX(150ms 节流)/ 离开 → FACE_LOST ---- */
static bool s_face_present;                 /* 边沿检测:只在"有→无"时发 LOST */

static void pub_face_lost(void)
{
    if (!s_face_present)
        return;
    s_face_present = false;
    EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
}

static void on_det(const RockIvaFaceDetResult *result,
                   const RockIvaExecuteStatus status, void *ud)
{
    (void)ud;
    if ((int)status != ROCKIVA_RET_SUCCESS || !result)
        return;
    if (result->objNum == 0) {
        /* 人脸离开:立即发(不节流),否则主页黄框会一直挂在屏上 */
        pub_face_lost();
        return;
    }

    static int64_t last_ms;
    int64_t now = now_ms();
    if (now - last_ms < 150)
        return;
    last_ms = now;
    s_face_present = true;

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

    feed_liveness(best);                /* 关键点回灌活体(B7 记账 / B8 判定) */
}

/* 命中发布前的总闸:口径一致(模型没换) + 活体通过(B7 pass 恒真) */
static bool publish_gate(void)
{
    if (!vision_service_features_compatible()) {
        static int n_stale;
        if (n_stale++ < 3)
            DG_LOGW(TAG, "特征口径与当前模型不一致:命中不下发(重新录入人脸后"
                         "更新 device_config.face_model_tag)");
        return false;
    }
    if (!cfg_get()->liveness_enable || liveness_service_pass())
        return true;
    DG_LOGW(TAG, "活体未通过,命中不下发");
    return false;
}

/* 1:1 比对(VERIFY_11):与模式目标特征比 → 命中发 EV_VISION_VERIFY_11
 * 只发成功:失败由 FSM 子步 5s 超时收口(spec-auth §4.4),
 * 否则同一段视频里逐帧的瞬时低分会把流程提前打死 */
static void verify_against_target(const char *feat, uint16_t flen)
{
    char uid[DG_UID_LEN];
    uint8_t target[DG_FEATURE_MAX];
    uint16_t tlen;
    vision_service_get_verify_uid(uid, sizeof(uid));
    if (!uid[0])
        return;                         /* 未指定目标:不比对 */

    pthread_mutex_lock(&s_target.mtx);
    tlen = (strcmp(s_target.uid, uid) == 0) ? s_target.len : 0;
    if (tlen)
        memcpy(target, s_target.data, tlen);
    pthread_mutex_unlock(&s_target.mtx);
    if (tlen < IVA_FEATURE_MIN)
        return;                         /* 目标无特征/未装载:等 FSM 5s 超时 */
    if (tlen != flen) {
        /* FeatureCompare 不接长度:不同长(换过模型/旧库残留)不可比,
         * 宁可比不出来也不读越界 */
        static int n_mismatch;
        if (n_mismatch++ < 3)
            DG_LOGW(TAG, "1:1 特征长度不一致(目标 %u B / 当前 %u B),跳过比对",
                    tlen, flen);
        return;
    }

    float score = 0;
    if (ROCKIVA_FACE_FeatureCompare(feat, target, &score) != ROCKIVA_RET_SUCCESS)
        return;
    if (score < cfg_get()->face_match_threshold || !publish_gate())
        return;

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (db_user_get(uid, &rec) != DG_OK)
        return;
    if (rec.role == DG_ROLE_BLACKLIST)
        return;                         /* 黑名单任何路径都失败(spec-auth §5) */

    ev_match_t m;
    memset(&m, 0, sizeof(m));
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "%s", rec.user_id);
    snprintf(m.user_name, sizeof(m.user_name), "%s", rec.user_name);
    m.role = rec.role;
    m.score_permille = (int32_t)(score * 1000.0f + 0.5f);
    EVENT_BUS_PUBLISH(EV_VISION_VERIFY_11, &m);
    DG_LOGI(TAG, "1:1 通过 %s(%s) %.3f", rec.user_id, rec.user_name, score);
}

/* 1:N 检索(DETECT_1N):库检索 → 阈值 + DB 存在性双确认 → EV_VISION_MATCH_1N */
static void search_1n(const char *feat, uint16_t flen)
{
    RockIvaFaceSearchResults sr;
    memset(&sr, 0, sizeof(sr));
    if (ROCKIVA_FACE_SearchFeature(IVA_LIB_NAME, feat, flen, 1, 1, &sr) !=
        ROCKIVA_RET_SUCCESS)
        return;
    if (sr.num < 1)
        return;
    const RockIvaFaceSearchResult *best = &sr.faceIdScore[0];
    float score = best->score;

    /* 最高分节流日志:板上调 face_match_threshold 的唯一依据(先看分再改值) */
    static int64_t last_log_ms;
    static float last_score = -1.0f;
    int64_t t = now_ms();
    if (t - last_log_ms >= IVA_SCORE_LOG_MS &&
        (score != last_score || score >= cfg_get()->face_match_threshold * 0.8f)) {
        last_log_ms = t;
        last_score = score;
        DG_LOGI(TAG, "1:N 最高分 %.3f(阈值 %.2f) 最近 %s", score,
                cfg_get()->face_match_threshold, best->faceIdInfo);
    }

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (score < cfg_get()->face_match_threshold ||
        db_user_get(best->faceIdInfo, &rec) != DG_OK)
        return;                         /* 分数不足 / 库与 DB 不同步(自愈) */
    if (!publish_gate())
        return;

    ev_match_t m;
    memset(&m, 0, sizeof(m));
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "%s", rec.user_id);
    snprintf(m.user_name, sizeof(m.user_name), "%s", rec.user_name);
    m.role = rec.role;
    m.score_permille = (int32_t)(score * 1000.0f + 0.5f);
    EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &m);
    DG_LOGI(TAG, "1:N 命中 %s(%s) %d‰", rec.user_id, rec.user_name,
            m.score_permille);
}

/* ---- 分析回调:特征 → 录入缓存 + 按模式检索/比对 ---- */
static void on_analyse(const RockIvaFaceCapResults *result,
                       const RockIvaExecuteStatus status, void *ud)
{
    (void)ud;
    if ((int)status != ROCKIVA_RET_SUCCESS || !result)
        return;

    dg_vision_mode_t mode = vision_service_get_mode();
    if (mode == DG_VMODE_IDLE)
        return;

    for (uint32_t i = 0; i < result->num; i++) {
        const RockIvaFaceCapResult *r = &result->faceResults[i];
        if (r->qualityResult != ROCKIVA_FACE_QUALITY_OK)
            continue;
        uint16_t flen = (uint16_t)r->faceAnalyseInfo.featureSize;
        const char *feat = r->faceAnalyseInfo.feature;
        if (flen < IVA_FEATURE_MIN)
            continue;
        /* 交接 §1.1:上板第一件事是确认实际特征长度。超限时明确报错,
         * 不静默丢弃(提高 proto/types.h DG_FEATURE_MAX 后重编译即可) */
        if (flen > DG_FEATURE_MAX) {
            static int n_over;
            if (n_over++ < 3)
                DG_LOGE(TAG, "特征 %u B > DG_FEATURE_MAX(%d),已丢弃——"
                             "请提高 proto/types.h DG_FEATURE_MAX 后重编译",
                        flen, DG_FEATURE_MAX);
            continue;
        }
        {
            static int logged_len;
            if (logged_len != (int)flen) {   /* 首次/变化时记录,联调可见 */
                logged_len = (int)flen;
                DG_LOGI(TAG, "人脸特征长度 %u B(上限 %d)", flen, DG_FEATURE_MAX);
            }
        }

        /* 缓存最新特征(录入取用;DETECT_ONLY 下也照常) */
        pthread_mutex_lock(&s_cap.mtx);
        memcpy(s_cap.data, feat, flen);
        s_cap.len = flen;
        s_cap.ms = now_ms();
        pthread_mutex_unlock(&s_cap.mtx);

        if (mode == DG_VMODE_VERIFY_11)
            verify_against_target(feat, flen);
        else if (mode == DG_VMODE_DETECT_1N)
            search_1n(feat, flen);
        /* DETECT_ONLY:只缓存,不检索(菜单/待机/验证子步) */
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

/* ---- 1:1 目标特征装载(模式钩子,总线线程调用) ---- */

static const char *s_target_want;           /* 遍历过滤器:目标 uid */

static int target_iter_cb(const char *user_id, const uint8_t *plain,
                          uint16_t len, void *ud)
{
    (void)ud;
    if (!s_target_want || strcmp(user_id, s_target_want) != 0)
        return 0;                           /* 继续找 */
    if (len < IVA_FEATURE_MIN || len > DG_FEATURE_MAX)
        return 1;                           /* 命中但特征不可用,中止 */
    pthread_mutex_lock(&s_target.mtx);
    memcpy(s_target.data, plain, len);
    s_target.len = len;
    snprintf(s_target.uid, sizeof(s_target.uid), "%s", user_id);
    pthread_mutex_unlock(&s_target.mtx);
    return 1;                               /* 已命中,中止遍历 */
}

static void clear_target(void)
{
    pthread_mutex_lock(&s_target.mtx);
    s_target.len = 0;
    s_target.uid[0] = '\0';
    memset(s_target.data, 0, sizeof(s_target.data));
    pthread_mutex_unlock(&s_target.mtx);
}

/* 模式变更:进入 VERIFY_11 时装目标特征,离开时清(明文不常驻) */
static void on_mode_changed(dg_vision_mode_t mode, const char *user_id)
{
    clear_target();
    if (mode == DG_VMODE_IDLE) {
        /* 关人脸后不再有回调:主动收掉主页脸框(无条件发,UI 收到即隐藏)。
         * 不动 s_face_present——它归检测回调线程独有(跨线程写会引入竞争) */
        EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
    }
    if (mode != DG_VMODE_VERIFY_11 || !user_id || !user_id[0])
        return;
    s_target_want = user_id;
    int rc = db_user_iter_face(target_iter_cb, NULL);
    s_target_want = NULL;
    pthread_mutex_lock(&s_target.mtx);
    uint16_t tlen = s_target.len;
    pthread_mutex_unlock(&s_target.mtx);
    if (rc != DG_OK || tlen == 0)
        DG_LOGW(TAG, "1:1 目标 %s 无可用人脸特征(等 FSM 5s 超时)", user_id);
    else
        DG_LOGI(TAG, "1:1 目标特征已装载 %s(%u B)", user_id, tlen);
}

/* ---- 查重比较器:1=重复 0=不重复 <0=错误(storage 契约) ---- */
static int rockiva_cmp(const uint8_t *a, uint16_t alen,
                       const uint8_t *b, uint16_t blen, void *ud)
{
    (void)ud;
    if (!a || !b || alen < IVA_FEATURE_MIN || blen < IVA_FEATURE_MIN)
        return -1;
    if (alen != blen) {
        /* FeatureCompare 不接长度;不同长(换过模型)不可比 → 不判重并告警,
         * 否则等于拿越界内存比对(误判重 = 用户录不进去) */
        static int n_mismatch;
        if (n_mismatch++ < 3)
            DG_LOGW(TAG, "查重特征长度不一致(%u B / %u B),跳过该行", alen, blen);
        return 0;
    }
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

/* 后端私有启动(契约义务与装配接线由服务层统一处理,见 vision_backend.h) */
static int rockiva_start(bool enable_mock)
{
    (void)enable_mock;                  /* 板上无 mock;该参数仅 PC sim 语义 */

    /* 本后端私有接线:录入抓取订阅 + 相机 NV12 出口 */
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req, NULL);
    camera_set_nv12_listener(on_frame_push, NULL);   /* 归还走 on_frames_release→camera_nv12_release */

    RockIvaInitParam ip;
    memset(&ip, 0, sizeof(ip));
    /* 日志级别:DG_IVA_LOG=<0 ERROR|1 WARN|2 DEBUG|3 INFO|4 TRACE>
     * 缺模型时用它看 ROCKIVA 到底想开哪些文件(联调/现场诊断) */
    const char *lv = getenv("DG_IVA_LOG");
    ip.logLevel = (lv && *lv) ? (RockIvaLogLevel)atoi(lv) : ROCKIVA_LOG_WARN;
    ip.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    /* 模型目录优先级:env DG_IVA_MODEL_DIR > cfg face.model_dir > /usr/lib
     * (env 留给现场诊断,cfg 是出厂配置;ROCKIVA 的 modelPath 是"目录",
     *  换模型 = 换该目录下的 .data 文件集,见 modules/vision/README.md) */
    const char *mdir = env_or("DG_IVA_MODEL_DIR", NULL);
    if (!mdir || !mdir[0])
        mdir = cfg_get()->face_model_dir[0] ? cfg_get()->face_model_dir : "/usr/lib";
    snprintf(ip.modelPath, sizeof(ip.modelPath), "%s", mdir);
    ip.imageInfo.width = 1280;
    ip.imageInfo.height = 720;
    ip.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;

    RockIvaRetCode rc = ROCKIVA_Init(&s_handle, ROCKIVA_MODE_VIDEO, &ip, NULL);
    if (rc != ROCKIVA_RET_SUCCESS) {
        DG_LOGE(TAG, "ROCKIVA_Init 失败(%d):模型目录 %s 缺人脸模型?"
                     "从 SDK external/iva/.../model 拷板后重启", rc, ip.modelPath);
        return DG_ERR_IO;               /* holder 依返回码置 ERROR(required=false 不退出) */
    }
    ROCKIVA_SetFrameReleaseCallback(s_handle, on_frames_release);

    RockIvaFaceTaskParams fp;
    memset(&fp, 0, sizeof(fp));
    fp.mode = ROCKIVA_FACE_MODE_NORMAL;
    fp.faceTaskType.faceCaptureEnable = 1;
    fp.faceTaskType.faceRecognizeEnable = 1;
    fp.faceTaskType.faceLandmarkEnable = 2;  /* 5 点 + 106 点(B8 活体几何量) */
    fp.faceCaptureRule.detectScore = 60;
    fp.faceCaptureRule.optType = ROCKIVA_FACE_OPT_FAST;
    fp.faceCaptureRule.faceQualityThrehold = 60;

    RockIvaFaceCallback cbs = { .detCallback = on_det,
                                .analyseCallback = on_analyse,
                                .postureCallback = NULL };
    rc = ROCKIVA_FACE_Init(s_handle, &fp, cbs);
    if (rc != ROCKIVA_RET_SUCCESS) {
        /* 实测(2026-09-18 板上 strace):FACE_Init 在 modelPath 下找
         * face_landmark5.data / face_quality_v2.data(以及识别模型),
         * 缺一个即失败——SDK 的 models/rockiva_data_rk3576 需整目录拷入 */
        DG_LOGE(TAG, "ROCKIVA_FACE_Init 失败(%d):人脸模型缺失?"
                     "%s 下需 face_landmark5.data / face_quality_v2.data 等"
                     "(把 SDK models/rockiva_data_rk3576/*.data 全量拷入;"
                     "DG_IVA_LOG=3 可看 ROCKIVA 找文件过程)", rc, ip.modelPath);
        ROCKIVA_Release(s_handle);
        s_handle = NULL;
        return DG_ERR_IO;
    }

    db_user_iter_face(lib_iter_cb, NULL);   /* 全量装载 DB 人脸特征 */

    s_ready = true;
    DG_LOGI(TAG, "ROCKIVA 就绪(model=%s,命中阈值 %.2f,查重阈值 %.2f)",
            ip.modelPath, cfg_get()->face_match_threshold,
            cfg_get()->face_dup_threshold);
    return DG_OK;
}

/* 后端注册项(装配层 app/main.c 注册;契约见 vision_backend.h)
 * model_tag:ROCKIVA 人脸的"特征口径"。换 /usr/lib 下的模型文件 = 换特征空间,
 * 必须同时改 face.model_tag,否则旧特征被静默当成可比 → 由此 tag 拦下。 */
const vision_backend_ops_t vision_backend_rockiva = {
    .name = "rockiva",
    .model_tag = "rockiva-face-v1",
    .has_landmarks = true,               /* faceLandmarkEnable=2 → 106 点给 B8 活体 */
    .start = rockiva_start,
    .lib_add = lib_add,
    .lib_del = lib_del,
    .compare = rockiva_cmp,
    .on_mode = on_mode_changed,
};
