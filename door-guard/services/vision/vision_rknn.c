/*
 * vision_rknn.c — 自组 rknn 人脸后端(RetinaFace 检测;识别接第二阶段)
 *
 * 链路(全部在相机线程内联完成,不新增线程:检测 6.7ms + RGA 亚毫秒,
 * 远小于 33ms 帧预算;且用完立刻归还 V4L2 缓冲,不会饿死 4 缓冲):
 *
 *   camera NV12 1280×720
 *     → RGA letterbox 320×320(等比缩放 + 补边 114)
 *     → RetinaFace@NPU(320×320,喂原始 uint8,归一化已烤进图)
 *     → rknn_retinaface_decode → NMS → 取最大脸
 *     → 逆映射 letterbox + 旋转映射到竖屏(720×1280)
 *     → EV_VISION_FACE_BOX(黄框)/ 无人脸 → EV_VISION_FACE_LOST
 *
 * 模型:RetinaFace_rk3576_i8.rknn(320×320 i8,板上实测 6.7ms;按 RK3576 重转,
 * 见 models/README.md ⑤)。路径 env DG_RKNN_MODEL_DIR > /userdata/doorguard/models。
 *
 * 分层:本文件只做"装配与事件发布"——推理在 drv/npu(npu_model/npu_pre),
 * 解码/对齐在 services/vision/rknn_face(纯 C 宿主可测)。三者各自可测,
 * 这里是唯一需要板上现场调的一段。
 *
 * 阶段说明:当前只出检测框。特征提取(ArcFace 112×112 → 512 维 → 余弦 1:N)
 * 是下一步,故 lib_add/lib_del/compare/on_mode 暂缺;model_tag 已按目标特征空间
 * 预先登记(接上识别即生效)。
 */
#include "vision_service.h"
#include "vision_backend.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "liveness_service.h"
#include "modules/camera/camera.h"

#include "npu_model.h"
#include "npu_pre.h"
#include "rknn_face.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[VISION]";

#define RKNN_DEFAULT_DIR   "/userdata/doorguard/models"
#define RKNN_DEFAULT_MODEL "RetinaFace_rk3576_i8.rknn"
#define RKNN_NMS_IOU       0.40f
#define RKNN_BOX_LOG_MS    2000     /* 检出分数日志节流(调阈值用) */
#define RKNN_PUB_MS        100      /* 脸框事件节流:UI 10Hz 足够,不刷爆总线 */
#define RKNN_MAX_CAND      256      /* 候选框上限(过阈后人脸数量级远小于此) */

static npu_model_t *s_face;
static bool s_ready;

static int        s_in_w, s_in_h;      /* 模型输入尺寸(320×320) */
static size_t     s_in_bytes;
static int        s_nanchor;           /* 锚框数(320 → 4200) */

static uint8_t   *s_rgb;               /* letterbox 后的输入画布 */
static float     *s_loc, *s_conf, *s_landm;
static rknn_face_t *s_cand;

static npu_letterbox_t s_lb;           /* 源→模型 的 letterbox 计划(源尺寸变化时重算) */
static int s_lb_src_w, s_lb_src_h;
static float s_score_thresh = 0.5f;    /* 检测阈值(实测调;见日志"最高分") */

static bool s_face_present;            /* 边沿检测:只在"有→无"发 FACE_LOST */

static const char *env_or(const char *k, const char *dflt)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : dflt;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pub_face_lost(void)
{
    if (!s_face_present)
        return;
    s_face_present = false;
    EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
}

/* 源图坐标(1280×720 横向)→ 竖屏(720×1280)。
 * 与 ROCKIVA 后端同一套映射(源宽高对调 + 顺 90° 旋转);若上板实测框镜像/
 * 转了 180°,把这里换成 ROT_270 公式(ox = y1)即可,不必动其它代码。 */
static void rect_to_screen(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int src_h,
                           int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh)
{
    *ox = src_h - y2;
    *oy = x1;
    *ow = y2 - y1;
    *oh = x2 - x1;
}

/* 5 关键点回灌活体(万分比坐标,与分辨率无关;ROCKIVA 后端给 106 点,
 * 这里 RetinaFace 给 5 点——B8 的 yaw/pitch 几何估计正是按 5 点设计的) */
static void feed_liveness(const rknn_face_t *f, int src_w, int src_h)
{
    dg_face_pt_t pts[RKNN_FACE_KPS];
    for (int i = 0; i < RKNN_FACE_KPS; i++) {
        pts[i].x = (int32_t)(f->kps[i][0] * 10000.0f / (float)src_w);
        pts[i].y = (int32_t)(f->kps[i][1] * 10000.0f / (float)src_h);
    }
    liveness_service_on_face(pts, RKNN_FACE_KPS, 0);
}

static void on_frame_push(const uint8_t *data, int w, int h, uint32_t frame_id)
{
    /* 未就绪/关人脸 → 立即归还(否则 4 缓冲耗尽,相机永久断流) */
    if (!s_ready || vision_service_get_mode() == DG_VMODE_IDLE) {
        camera_nv12_release(frame_id);
        return;
    }
    if (w <= 0 || h <= 0) {
        camera_nv12_release(frame_id);
        return;
    }

    /* 源尺寸变化时重算 letterbox 计划(正常恒为 1280×720) */
    if (w != s_lb_src_w || h != s_lb_src_h) {
        npu_letterbox_plan(w, h, s_in_w, s_in_h, &s_lb);
        s_lb_src_w = w;
        s_lb_src_h = h;
        DG_LOGI(TAG, "letterbox 计划 %dx%d → %dx%d(scale=%.4f,补边 %d,%d)",
                w, h, s_in_w, s_in_h, s_lb.scale, s_lb.pad_x, s_lb.pad_y);
    }

    int rc = npu_pre_nv12_letterbox_rgb(data, w, &s_lb, s_rgb);
    if (rc != DG_OK) {
        camera_nv12_release(frame_id);
        return;
    }
    /* 模型输入是 I8,但补的是原始 uint8 图像 → 声明 U8,由运行时按量化参数转换 */
    rc = npu_model_run(s_face, s_rgb, s_in_bytes, DG_NPU_TYPE_U8);
    if (rc != DG_OK) {
        camera_nv12_release(frame_id);
        return;
    }

    /* 取三个输出(驱动已反量化) */
    uint32_t elems = 0;
    int ok = npu_model_output_f32(s_face, 0, s_loc, (uint32_t)s_nanchor * 4, &elems) == DG_OK &&
             npu_model_output_f32(s_face, 1, s_conf, (uint32_t)s_nanchor * 2, &elems) == DG_OK &&
             npu_model_output_f32(s_face, 2, s_landm, (uint32_t)s_nanchor * 10, &elems) == DG_OK;
    /* 缓冲用完立即归还:后面都是纯计算,不必占着 V4L2 缓冲 */
    camera_nv12_release(frame_id);
    if (!ok) {
        DG_LOGE(TAG, "取输出失败(模型输出数与预期不符?)");
        return;
    }

    int n = rknn_retinaface_decode(s_loc, s_conf, s_landm, s_nanchor, s_in_w,
                                   s_score_thresh, s_cand, RKNN_MAX_CAND);
    if (n < 0) {
        DG_LOGE(TAG, "解码失败(%d):锚框数与模型输入不匹配?", n);
        return;
    }
    if (n > RKNN_MAX_CAND) {
        DG_LOGW(TAG, "候选框 %d 超出容量 %d,已截断", n, RKNN_MAX_CAND);
        n = RKNN_MAX_CAND;
    }

    n = rknn_nms(s_cand, n, RKNN_NMS_IOU);
    if (n <= 0) {
        pub_face_lost();
        return;
    }

    /* 取最大脸(门口通常单人);框已按分数降序,但面积另比 */
    int best = 0;
    float best_area = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = (s_cand[i].x2 - s_cand[i].x1) * (s_cand[i].y2 - s_cand[i].y1);
        if (a > best_area) {
            best_area = a;
            best = i;
        }
    }

    /* 模型输入坐标 → 源图坐标(逆 letterbox) */
    float sx1, sy1, sx2, sy2;
    npu_letterbox_unmap(&s_lb, s_cand[best].x1, s_cand[best].y1, &sx1, &sy1);
    npu_letterbox_unmap(&s_lb, s_cand[best].x2, s_cand[best].y2, &sx2, &sy2);
    /* 夹到源图内(逆映射后可能越界,画到屏外会很难看) */
    if (sx1 < 0) sx1 = 0;
    if (sy1 < 0) sy1 = 0;
    if (sx2 > (float)w) sx2 = (float)w;
    if (sy2 > (float)h) sy2 = (float)h;

    /* 关键点回灌活体(用源图坐标系) */
    {
        rknn_face_t fsrc = s_cand[best];
        float kx, ky;
        for (int i = 0; i < RKNN_FACE_KPS; i++) {
            npu_letterbox_unmap(&s_lb, s_cand[best].kps[i][0], s_cand[best].kps[i][1], &kx, &ky);
            fsrc.kps[i][0] = kx;
            fsrc.kps[i][1] = ky;
        }
        feed_liveness(&fsrc, w, h);
    }

    /* 节流日志:板上调 s_score_thresh 的依据(先看分再改值) */
    {
        static int64_t last_log;
        static float last_score = -1.0f;
        const int64_t t = now_ms();
        if (t - last_log >= RKNN_BOX_LOG_MS && s_cand[best].score != last_score) {
            last_log = t;
            last_score = s_cand[best].score;
            DG_LOGI(TAG, "检出 %d 张脸,最大脸分数 %.3f(阈值 %.2f,%.0fx%.0f)",
                    n, s_cand[best].score, s_score_thresh,
                    sx2 - sx1, sy2 - sy1);
        }
    }

    /* 脸框事件节流:UI 10Hz 足够,不必每帧刷总线 */
    {
        static int64_t last_pub;
        const int64_t t = now_ms();
        if (t - last_pub < RKNN_PUB_MS)
            return;
        last_pub = t;
    }

    ev_face_box_t box;
    memset(&box, 0, sizeof(box));
    box.state = DG_BOX_DETECTED;
    rect_to_screen((int32_t)sx1, (int32_t)sy1, (int32_t)sx2, (int32_t)sy2, h,
                   &box.x, &box.y, &box.w, &box.h);
    s_face_present = true;
    EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
}

/* 录入抓取请求:阶段一尚未接特征提取,明确告警而不是静默不响应
 * (静默会让用户在录入页干等,以为是设备卡了) */
static int on_capture_req(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    static int n_warn;
    if (n_warn++ < 3)
        DG_LOGW(TAG, "录入取特征:当前后端仅检测(识别未接入),本次录入不会有特征");
    return 0;
}

static void on_mode_changed(dg_vision_mode_t mode, const char *user_id)
{
    (void)user_id;
    if (mode == DG_VMODE_IDLE) {
        /* 关人脸后不再有回调:主动收掉主页脸框 */
        EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
        s_face_present = false;
    }
}

static int rknn_start(bool enable_mock)
{
    (void)enable_mock;

    const char *dir = env_or("DG_RKNN_MODEL_DIR", RKNN_DEFAULT_DIR);
    const char *fname = env_or("DG_RKNN_FACE_MODEL", RKNN_DEFAULT_MODEL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);

    DG_LOGI(TAG, "rknn 后端启动:运行时 %s", npu_hal_version());

    s_face = npu_model_load(path);
    if (!s_face)
        return DG_ERR_IO;               /* holder 记 ERROR,降级为"无检测" */

    /* 尺寸与锚框数从模型查出来,并与解码器的期望互校——不符就在启动时响亮失败,
     * 而不是每帧给出一堆错框(启动即失败比运行期静默错误好定位得多) */
    npu_attr_t in;
    if (npu_model_input_attr(s_face, 0, &in) != DG_OK) {
        DG_LOGE(TAG, "查输入属性失败");
        goto fail;
    }
    s_in_w = in.width;
    s_in_h = in.height;
    s_in_bytes = in.nbytes;

    if (npu_model_output_num(s_face) != 3) {
        DG_LOGE(TAG, "输出应为 3(框/分/关键点),实际 %u——不是 RetinaFace 模型?",
                npu_model_output_num(s_face));
        goto fail;
    }
    npu_attr_t o0;
    if (npu_model_output_attr(s_face, 0, &o0) != DG_OK) {
        DG_LOGE(TAG, "查输出属性失败");
        goto fail;
    }
    s_nanchor = (o0.n_dims >= 2) ? (int)o0.dims[1] : 0;
    const int want = rknn_retinaface_anchor_count(s_in_w);
    if (s_nanchor != want) {
        DG_LOGE(TAG, "锚框数 %d 与 %d 输入期望的 %d 不符——解码方案与模型不匹配",
                s_nanchor, s_in_w, want);
        goto fail;
    }

    s_rgb   = malloc((size_t)s_in_w * s_in_h * 3);
    s_loc   = malloc(sizeof(float) * (size_t)s_nanchor * 4);
    s_conf  = malloc(sizeof(float) * (size_t)s_nanchor * 2);
    s_landm = malloc(sizeof(float) * (size_t)s_nanchor * 10);
    s_cand  = malloc(sizeof(rknn_face_t) * RKNN_MAX_CAND);
    if (!s_rgb || !s_loc || !s_conf || !s_landm || !s_cand) {
        DG_LOGE(TAG, "缓冲分配失败");
        goto fail;
    }

    /* 本后端私有接线:相机 NV12 出口 + 录入请求订阅 */
    camera_set_nv12_listener(on_frame_push, NULL);
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req, NULL);

    s_ready = true;
    DG_LOGI(TAG, "rknn 就绪:RetinaFace %dx%d,锚框 %d,检出阈值 %.2f(%s)",
            s_in_w, s_in_h, s_nanchor, s_score_thresh, path);
    return DG_OK;

fail:
    /* 半初始化失败要收干净:否则 holder 记 ERROR 但资源还挂着 */
    if (s_face) {
        npu_model_release(s_face);
        s_face = NULL;
    }
    free(s_rgb);   s_rgb = NULL;
    free(s_loc);   s_loc = NULL;
    free(s_conf);  s_conf = NULL;
    free(s_landm); s_landm = NULL;
    free(s_cand);  s_cand = NULL;
    return DG_ERR_IO;
}

/* 后端注册项(装配层 app/main.c 注册;契约见 vision_backend.h)。
 * model_tag:自组路线的特征口径——ArcFace-R50 512 维(与 ROCKIVA 的
 * rockiva-face-v1 是**不同的特征空间**,两边特征互不可比)。接上识别后生效。 */
const vision_backend_ops_t vision_backend_rknn = {
    .name = "rknn",
    .model_tag = "rknn-arcface-r50-v1",
    .has_landmarks = true,              /* 5 点关键点(RetinaFace 输出),供 B8 几何活体 */
    .start = rknn_start,
    .lib_add = NULL,                    /* 阶段一:识别未接,无特征库维护 */
    .lib_del = NULL,
    .compare = NULL,
    .on_mode = on_mode_changed,
};
