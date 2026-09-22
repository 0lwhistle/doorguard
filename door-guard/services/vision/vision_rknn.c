/*
 * vision_rknn.c — 自组 rknn 人脸后端(RetinaFace 检测 + ArcFace 识别)
 *
 * 链路(推理在专用 worker 线程;取流/预览/LVGL 主循环零等待):
 *
 *   camera NV12 1280×720(主循环 camera_poll 投递)
 *     └─ on_frame_push 只做"信箱投递"(最新帧;worker 忙时顶掉旧帧立即归还)——
 *        回调在主循环执行,绝不允许阻塞。
 *     worker 线程取帧:
 *     ├─[每帧] **RGA 旋到预览同向**(横置摄像头原始帧里人是躺着的,模型只认
 *     │        正立脸;旋转后框/关键点与预览=屏幕同域,映射恒等)→
 *     │        RGA letterbox 320×320 → RetinaFace@NPU(6.7ms)→ 解码 → NMS
 *     │        → 最大脸 → 逆映射(屏幕域)→ EV_VISION_FACE_BOX(黄框,15Hz 节流)
 *     └─[每 300ms 且非 IDLE] 从旋转帧裁人脸 ROI(RGA,正方形)→ 5 点对齐
 *              112×112 → (x-127.5)/127.5 → ArcFace@NPU(56ms)→ 512 维 → L2
 *              → 录入缓存 / 1:1 比对 / 1:N 检索(内存特征库暴力余弦)
 *
 *   worker 处理一帧最坏 ≈ 7+56+RGA ≈ 65ms → 满载约 15fps:信箱永远只留
 *   最新帧(处理期间新到的帧顶掉旧帧立即归还),识别/画框永远基于新鲜帧,
 *   V4L2 4 缓冲同时最多占 2(信箱 1 + 处理中 1),不会饿死相机。
 *   注意:letterbox/crop 与 camera 模块的预览旋转并发使用 RGA——librga
 *   按调用走独立请求,多线程并发是它的正常用法。
 *
 * 模型(models/README.md ⑤,均在板上 /userdata/doorguard/models):
 *   RetinaFace_rk3576_i8.rknn  320×320 I8,归一化已烤进图,喂原始 U8;
 *   w600k_r50.rknn             112×112 F16,**未烤归一化**,必须喂
 *                              (x-127.5)/127.5 的 F32——板上实测(u8 模式
 *                              cos(脸,纯色)=0.79 全部相似;f32 模式 0.11 正常)。
 *
 * 分层:推理在 drv/npu,解码/对齐/余弦在 rknn_face(纯 C 宿主可测),
 * 本文件只做装配、调度与事件发布。
 *
 * 阈值:沿用 cfg face.match_threshold(1:N/1:1)与 face.dup_threshold(查重)。
 * 注意余弦分度与 ROCKIVA 不同,0.42 是起点,按 2s 节流日志实测标定(遗留)。
 */
#include "vision_service.h"
#include "vision_backend.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "liveness_service.h"
#include "modules/camera/camera.h"
#include "modules/jpeg/dg_jpeg.h"
#include "storage.h"
#include "cfg.h"

#include "npu_model.h"
#include "npu_pre.h"
#include "rknn_face.h"
#include "face_quality.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[VISION]";

#define RKNN_DEFAULT_DIR    "/userdata/doorguard/models"
#define RKNN_FACE_MODEL     "RetinaFace_rk3576_i8.rknn"
#define RKNN_REC_MODEL      "w600k_r50.rknn"
#define RKNN_REC_DIM        512         /* ArcFace-R50 输出维度(板上实测) */
#define RKNN_FEATURE_BYTES  (RKNN_REC_DIM * 4)   /* 2048 B = DG_FEATURE_MAX */
#define RKNN_FEATURE_MIN    16          /* 低于视为无效特征(字节) */
#define RKNN_NMS_IOU        0.40f
#define RKNN_MAX_CAND       256         /* 候选框上限 */
#define RKNN_PUB_MS         66          /* 脸框事件节流(15Hz;10Hz 跟手性不足) */
#define RKNN_BOX_LOG_MS     2000        /* 检出分数日志节流(调阈值用) */
#define RKNN_SCORE_LOG_MS   2000        /* 1:N 最高分日志节流 */
#define RKNN_REC_MS         300         /* 识别节流(录入缓存新鲜度 ≤300ms) */
#define RKNN_ROI_MAX        256         /* ROI 裁剪输出上限(边长) */
/* ROI 相对关键点外扩系数。112/160 对齐画布映射回源图约要关键点外接框的
 * 2.5~3 倍,1.5 会让 warp 采样越出 ROI(头像四角发黑、下巴/额头被裁);
 * 2.2 已能盖住脸本体+大半画布,再大只剩黑边没增益 */
#define RKNN_ROI_MARGIN     2.2f
#define RKNN_AVATAR_SZ      160         /* 头像边长(列表 40px/预览 160px 都够) */
#define RKNN_Q_PUB_MS       1000        /* 质量事件兜底刷新(拍摄页 UI 状态) */

static npu_model_t *s_face, *s_rec;
static bool s_ready;

/* 检测侧 */
static int         s_in_w, s_in_h, s_nanchor;
static size_t      s_in_bytes;
static uint8_t    *s_rgb;
static uint8_t    *s_rot;              /* 旋到预览同向的 NV12 帧(worker 独占,
                                          首帧按相机实际幅面按需分配) */
static size_t      s_rot_cap;          /* s_rot 容量(字节) */
static float      *s_loc, *s_conf, *s_landm;
static rknn_face_t *s_cand;
static npu_letterbox_t s_lb;
static int s_lb_src_w, s_lb_src_h;
static bool s_face_present;            /* worker 写;on_mode_changed(总线线程)置
                                          false 仅单字写,读侧滞后一帧无实义 */
static int64_t s_last_det_ms;          /* 最近一次检出的时刻:LOST 滞回用 */
/* 一次在场只放行一次:1:N 是持续上报的,人站在镜头前会每 300ms 命中一次。
 * 不设这道闸,FSM 就会开门→结果→回普通→再开门地循环:继电器反复动作、
 * 日志刷屏、弹窗反复建销(把 UI 拖垮 = 主页面卡死),脸框也因状态反复切换
 * 而闪烁。语义:走开(发 FACE_LOST)再回来 = 新的一次,可以再开。*/ 
static bool s_granted_presence;

/* 最近一次质量测量与判定:供日志标定 + 拍摄页实时提示(下一班接入 UI) */
static face_quality_t s_last_q;
static int32_t        s_last_q_verdict;

/* 识别侧 */
static int      s_rec_dim;            /* 实际输出维度(=512 时才启用识别) */
static size_t   s_rec_in_bytes;
static uint8_t *s_roi;                /* 人脸 ROI(RGB) */
static uint8_t *s_aligned;            /* 对齐后 112×112(RGB) */
static uint8_t  s_avatar_warp[RKNN_AVATAR_SZ * RKNN_AVATAR_SZ * 3];
                                       /* 头像 160×160 对齐结果(worker 线程独占) */
static float   *s_norm_in;            /* ArcFace 输入(归一化 f32) */
static int64_t  s_last_rec_ms;

/* ---- worker 线程:推理重活全部离开主循环(UI 卡顿的根因,2026-09-22) ----
 * 信箱容量 1:主循环回调只投"最新帧编号",worker 忙时新帧顶掉旧帧并立即
 * 归还旧帧(零拷贝契约:V4L2 缓冲要么在处理要么已归还,不积压)。 */
static pthread_mutex_t s_mb_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_mb_cond = PTHREAD_COND_INITIALIZER;
static struct {
    const uint8_t *data;                /* NV12 首址(V4L2 缓冲,归还前有效) */
    uint32_t fid;                       /* V4L2 缓冲 frame_id */
    int      w, h;
    bool     has;
} s_mb;

/* 最新「特征 + 同帧头像大图」成对缓存(analyse 路径同帧写入;录入
 * CAPTURE_REQ 读)。一把锁保护成对性:照片与特征必须出自同一帧,
 * 否则脸动了一下,头像就和特征对不上号(拍摄录入交接 §3.1 硬要求)。 */
static pthread_mutex_t s_cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint8_t  data[DG_FEATURE_MAX];
    uint16_t len;
    int64_t  ms;
} s_cap;
static struct {
    uint8_t  data[RKNN_AVATAR_SZ * RKNN_AVATAR_SZ * 3];
    int64_t  ms;                        /* 恒等于同帧 s_cap.ms */
} s_snap;

/* 1:1 目标特征(模式切换时装载) */
static struct {
    pthread_mutex_t mtx;
    uint8_t  data[DG_FEATURE_MAX];
    uint16_t len;
    char     uid[DG_UID_LEN];
} s_target = { .mtx = PTHREAD_MUTEX_INITIALIZER };

/* 特征库(启动全量装载 + 增删维护;检索=纯内存余弦,2000×512 点积毫秒级) */
static struct {
    pthread_mutex_t mtx;
    char  uid[DG_USER_MAX][DG_UID_LEN];
    float vec[DG_USER_MAX][RKNN_REC_DIM];
    int   n;
} s_lib = { .mtx = PTHREAD_MUTEX_INITIALIZER };

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
    s_granted_presence = false;         /* 人走了:下次来算新的一次 */
    EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
}

/* 源图坐标已在**预览/屏幕域**(worker 先把帧旋到与预览同向再检测,
 * 见 process_frame):模型空间逆映射出来的框就是屏幕像素,直接发布。
 * 此前"逆映射到横向原始帧再 rect_to_screen 转 90°"的两段式映射已删——
 * 域一多就出对不齐的 bug,让检测域=显示域,错误无法表示。 */

/* 5 关键点回灌活体(万分比坐标;B8 的 yaw/pitch 几何估计按 5 点设计) */
static void feed_liveness(const rknn_face_t *f, int src_w, int src_h)
{
    dg_face_pt_t pts[RKNN_FACE_KPS];
    for (int i = 0; i < RKNN_FACE_KPS; i++) {
        pts[i].x = (int32_t)(f->kps[i][0] * 10000.0f / (float)src_w);
        pts[i].y = (int32_t)(f->kps[i][1] * 10000.0f / (float)src_h);
    }
    liveness_service_on_face(pts, RKNN_FACE_KPS, 0);
}

/* 命中发布前的总闸(与 ROCKIVA 后端同语义):口径一致 + 活体通过 */
static bool publish_gate(void)
{
    if (!vision_service_features_compatible()) {
        static int n_stale;
        if (n_stale++ < 3)
            DG_LOGW(TAG, "特征口径与当前模型不一致:命中不下发(重录人脸后更新"
                         " face_model_tag)");
        return false;
    }
    if (!cfg_get()->liveness_enable || liveness_service_pass())
        return true;
    DG_LOGW(TAG, "活体未通过,命中不下发");
    return false;
}

/* ---- 特征库维护 ---------------------------------------------------------- */

static int lib_del(const char *user_id);   /* lib_add 的覆盖语义要先删后加 */

static int lib_load_cb(const char *user_id, const uint8_t *plain,
                       uint16_t len, void *ud)
{
    (void)ud;
    if (len != RKNN_FEATURE_BYTES) {
        DG_LOGW(TAG, "特征库装载 %s:长度 %u ≠ %d(旧模型特征?),跳过",
                user_id, len, RKNN_FEATURE_BYTES);
        return 0;
    }
    pthread_mutex_lock(&s_lib.mtx);
    if (s_lib.n < DG_USER_MAX) {
        snprintf(s_lib.uid[s_lib.n], DG_UID_LEN, "%s", user_id);
        memcpy(s_lib.vec[s_lib.n], plain, RKNN_FEATURE_BYTES);
        s_lib.n++;
    }
    pthread_mutex_unlock(&s_lib.mtx);
    return 0;
}

static int lib_add(const char *user_id, const uint8_t *feature, uint16_t len)
{
    if (!s_ready || len != RKNN_FEATURE_BYTES)
        return DG_ERR_PARAM;
    lib_del(user_id);                       /* 重复添加 = 覆盖(幂等) */
    pthread_mutex_lock(&s_lib.mtx);
    int rc = DG_ERR_NO_MEMORY;
    if (s_lib.n < DG_USER_MAX) {
        snprintf(s_lib.uid[s_lib.n], DG_UID_LEN, "%s", user_id);
        memcpy(s_lib.vec[s_lib.n], feature, len);
        s_lib.n++;
        rc = DG_OK;
    }
    pthread_mutex_unlock(&s_lib.mtx);
    return rc;
}

static int lib_del(const char *user_id)
{
    pthread_mutex_lock(&s_lib.mtx);
    for (int i = 0; i < s_lib.n; i++) {
        if (strcmp(s_lib.uid[i], user_id) == 0) {
            s_lib.uid[i][0] = '\0';
            /* 尾行补位,保持紧凑 */
            s_lib.n--;
            if (i != s_lib.n) {
                memcpy(s_lib.uid[i], s_lib.uid[s_lib.n], DG_UID_LEN);
                memcpy(s_lib.vec[i], s_lib.vec[s_lib.n], sizeof(s_lib.vec[0]));
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_lib.mtx);
    return DG_OK;                           /* 删不存在 = 成功(契约) */
}

/* 查重比较器:余弦 ≥ face.dup_threshold 判重(1=重复 0=不重复 <0=错误) */
static int rknn_cmp(const uint8_t *a, uint16_t alen,
                    const uint8_t *b, uint16_t blen, void *ud)
{
    (void)ud;
    if (!a || !b || alen != RKNN_FEATURE_BYTES || blen != RKNN_FEATURE_BYTES)
        return -1;
    const float score = rknn_cosine((const float *)a, (const float *)b, RKNN_REC_DIM);
    return score >= cfg_get()->face_dup_threshold ? 1 : 0;
}

/* ---- 1:1 / 1:N ----------------------------------------------------------- */

static void verify_against_target(const float *feat)
{
    char uid[DG_UID_LEN];
    float target[RKNN_REC_DIM];
    uint16_t tlen;
    vision_service_get_verify_uid(uid, sizeof(uid));
    if (!uid[0])
        return;

    pthread_mutex_lock(&s_target.mtx);
    tlen = (strcmp(s_target.uid, uid) == 0) ? s_target.len : 0;
    if (tlen == RKNN_FEATURE_BYTES)
        memcpy(target, s_target.data, RKNN_FEATURE_BYTES);
    pthread_mutex_unlock(&s_target.mtx);
    if (tlen != RKNN_FEATURE_BYTES)
        return;                             /* 目标无特征/未装载:等 FSM 5s 超时 */

    const float score = rknn_cosine(feat, target, RKNN_REC_DIM);
    if (score < cfg_get()->face_match_threshold || !publish_gate())
        return;

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (db_user_get(uid, &rec) != DG_OK)
        return;
    if (rec.role == DG_ROLE_BLACKLIST)
        return;                             /* 黑名单任何路径都失败 */

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

static void search_1n(const float *feat)
{
    pthread_mutex_lock(&s_lib.mtx);
    int best_i = -1;
    float best = -2.0f;
    for (int i = 0; i < s_lib.n; i++) {
        const float s = rknn_cosine(feat, s_lib.vec[i], RKNN_REC_DIM);
        if (s > best) {
            best = s;
            best_i = i;
        }
    }
    char best_uid[DG_UID_LEN] = "";
    if (best_i >= 0)
        snprintf(best_uid, sizeof(best_uid), "%s", s_lib.uid[best_i]);
    pthread_mutex_unlock(&s_lib.mtx);

    if (best_i < 0)
        return;                             /* 空库 */

    /* 最高分节流日志:板上调 face.match_threshold 的唯一依据 */
    static int64_t last_log;
    static float last_score = -1.0f;
    const int64_t t = now_ms();
    if (t - last_log >= RKNN_SCORE_LOG_MS &&
        (best != last_score || best >= cfg_get()->face_match_threshold * 0.8f)) {
        last_log = t;
        last_score = best;
        DG_LOGI(TAG, "1:N 最高分 %.3f(阈值 %.2f) 最近 %s", best,
                cfg_get()->face_match_threshold, best_uid);
    }

    if (s_granted_presence) {
        static int64_t last_log;
        const int64_t t = now_ms();
        if (t - last_log >= 5000) {
            last_log = t;
            DG_LOGI(TAG, "本次在场已放行过,忽略重复命中(人离开后再来才会再开)");
        }
        return;
    }

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (best < cfg_get()->face_match_threshold ||
        db_user_get(best_uid, &rec) != DG_OK)
        return;                             /* 分数不足 / 库与 DB 不同步(自愈) */
    if (!publish_gate())
        return;
    if (rec.role == DG_ROLE_BLACKLIST)
        return;

    ev_match_t m;
    memset(&m, 0, sizeof(m));
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "%s", rec.user_id);
    snprintf(m.user_name, sizeof(m.user_name), "%s", rec.user_name);
    m.role = rec.role;
    m.score_permille = (int32_t)(best * 1000.0f + 0.5f);
    s_granted_presence = true;              /* 本次在场不再重复放行 */
    EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &m);
    DG_LOGI(TAG, "1:N 命中 %s(%s) %d‰", rec.user_id, rec.user_name,
            m.score_permille);
}

/* ---- 识别路径:ROI 裁剪 → 对齐 → 归一化 → ArcFace ------------------------ */

static bool recognize(const uint8_t *nv12, int w, int h,
                      const rknn_face_t *src /* 预览/屏幕域坐标 */, float src_score,
                      float *feat_out)
{
    /* ROI:关键点外接框外扩的**正方形**,整体平移夹进帧内(偶对齐)。
     * 不能各边独立裁剪缩小:rw≠rh 会让 kps 被各向异性挤压(sx≠sy),
     * 相似变换拟合在畸变坐标上,对齐结果整体歪斜——头像「歪 45°」、识别
     * 分数不稳的根因(2026-09-22 修)。正方形装不下时整块平移,仍装不下
     * (脸比画面还大)才缩边,且缩完仍是正方形 */
    float kmin_x = 1e9f, kmin_y = 1e9f, kmax_x = -1e9f, kmax_y = -1e9f;
    for (int i = 0; i < RKNN_FACE_KPS; i++) {
        if (src->kps[i][0] < kmin_x) kmin_x = src->kps[i][0];
        if (src->kps[i][0] > kmax_x) kmax_x = src->kps[i][0];
        if (src->kps[i][1] < kmin_y) kmin_y = src->kps[i][1];
        if (src->kps[i][1] > kmax_y) kmax_y = src->kps[i][1];
    }
    const float cx = (kmin_x + kmax_x) * 0.5f;
    const float cy = (kmin_y + kmax_y) * 0.5f;
    const float span = (kmax_x - kmin_x > kmax_y - kmin_y ? kmax_x - kmin_x
                                                          : kmax_y - kmin_y)
                       * RKNN_ROI_MARGIN;
    int side = (int)(span);
    if (side < 64)
        return false;                       /* 脸太小,识别无意义(与旧 half<32 同口径) */
    if (side > w) side = w;
    if (side > h) side = h;

    int rx = (int)cx - side / 2, ry = (int)cy - side / 2;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + side > w) rx = w - side;
    if (ry + side > h) ry = h - side;
    side &= ~1;                             /* YUV420 裁剪须偶数(只会更小,仍居中) */
    rx &= ~1; ry &= ~1;
    if (side < 32)
        return false;

    const int dw = side < RKNN_ROI_MAX ? side : RKNN_ROI_MAX;
    if (npu_pre_nv12_crop_rgb(nv12, w, w, h, rx, ry, side, side, s_roi, dw, dw) != DG_OK)
        return false;

    /* 关键点映射进 ROI 坐标(正方形 ROI ⇒ sx==sy,无畸变),求相似变换 */
    float kps_roi[RKNN_FACE_KPS][2];
    const float ks = (float)dw / (float)side;
    for (int i = 0; i < RKNN_FACE_KPS; i++) {
        kps_roi[i][0] = (src->kps[i][0] - (float)rx) * ks;
        kps_roi[i][1] = (src->kps[i][1] - (float)ry) * ks;
    }
    float m[6];
    if (rknn_align_plan(kps_roi, m) != 0)
        return false;
    rknn_align_warp(s_roi, dw, dw, m, s_aligned, 112, 112);

    /* ---- 质量闸门:测的正是"要喂给识别的那张脸" ----
     * 抖动糊脸喂进去会得到不可信特征——既可能误判,也会污染库(录进糊脸,
     * 以后本人刷不开)。阈值全走配置(face_quality.h;0 = 该项不启用)。 */
    {
        static uint8_t s_gray[112 * 112];
        face_quality_gray(s_aligned, 112, 112, s_gray);
        face_quality_thr_t thr;
        thr.min_face_px = cfg_get()->face_min_px;
        thr.blur_min = cfg_get()->face_blur_min;
        thr.det_score_min = cfg_get()->face_det_score_min;

        face_quality_t q;
        const int32_t bw_px = (int32_t)(src->x2 - src->x1);
        const int32_t bh_px = (int32_t)(src->y2 - src->y1);
        q.face_px = bw_px < bh_px ? bw_px : bh_px;   /* 源图上人脸框较小边 */
        q.det_score = src_score;
        q.blur = face_quality_blur(s_gray, 112, 112);

        const face_quality_verdict_t v = face_quality_check(&q, &thr);
        s_last_q = q;
        s_last_q_verdict = v;

        /* 质量事件(拍摄页实时提示用):verdict 变化即发;不变时 1s 兜底
         * 刷一次——拍摄页 push 后最多 1s 内能拿到当前状态,平时近零开销 */
        {
            static face_quality_verdict_t last_pub_v = FQ_ERR_PARAM;
            static int64_t last_pub_ms;
            const int64_t now = now_ms();
            if (v != last_pub_v || now - last_pub_ms >= RKNN_Q_PUB_MS) {
                last_pub_v = v;
                last_pub_ms = now;
                ev_vision_quality_t eq;
                memset(&eq, 0, sizeof(eq));
                eq.verdict = v;
                eq.face_px = q.face_px;
                EVENT_BUS_PUBLISH(EV_VISION_QUALITY, &eq);
            }
        }

        if (v != FQ_OK) {
            /* 节流日志:板上标定阈值的依据(先看实测值再改配置) */
            static int64_t last_log;
            const int64_t t = now_ms();
            if (t - last_log >= 2000) {
                last_log = t;
                DG_LOGI(TAG, "质量闸门拦下(%s):脸 %dpx 清晰度 %.0f 检测分 %.2f"
                             "(阈值 %d/%0.f/%.2f)",
                        face_quality_reason_str(v), q.face_px, q.blur, q.det_score,
                        thr.min_face_px, thr.blur_min, thr.det_score_min);
            }
            return false;
        }
    }

    /* 头像大图:按 AVATAR/112 整体缩放对齐矩阵再采样一次(M'=S·M,复用同一
     * 份对齐结果,全库头像构图一致);与特征同帧,由调用方成对入缓存 */
    {
        float mb[6];
        const float s = (float)RKNN_AVATAR_SZ / 112.0f;
        for (int i = 0; i < 6; i++)
            mb[i] = m[i] * s;
        rknn_align_warp(s_roi, dw, dw, mb, s_avatar_warp, RKNN_AVATAR_SZ,
                        RKNN_AVATAR_SZ);
        /* 诊断日志(节流):对齐拟合出的面内旋转角。头像歪斜/识别分数异常时
         * 第一时间看这里——检测输入已旋到正立域,正常应 ≈0°(即被摄者头部
         * 的自然倾角,±10° 内);明显偏离=关键点或模板出了问题 */
        static int64_t last_ang_log;
        const int64_t now = now_ms();
        if (now - last_ang_log >= RKNN_BOX_LOG_MS) {
            last_ang_log = now;
            const float deg = atan2f(m[3], m[0]) * (180.0f / 3.14159265f);
            DG_LOGI(TAG, "对齐旋转角 %.1f°(ROI %dpx,缩放 %.2f)", deg, side, ks);
        }
    }

    /* 归一化 → NPU → L2(板上实测:该模型未烤归一化,必须喂 (x-127.5)/127.5) */
    rknn_rgb_norm_f32(s_aligned, 112 * 112, s_norm_in);
    if (npu_model_run(s_rec, s_norm_in, s_rec_in_bytes, DG_NPU_TYPE_F32) != DG_OK)
        return false;
    uint32_t n = 0;
    if (npu_model_output_f32(s_rec, 0, feat_out, RKNN_REC_DIM, &n) != DG_OK ||
        n != (uint32_t)s_rec_dim)
        return false;
    rknn_l2_normalize(feat_out, s_rec_dim);
    return true;
}

/* ---- 帧入口 -------------------------------------------------------------- */

static void process_frame(const uint8_t *data, int w, int h, uint32_t frame_id);

/* 主循环回调(camera_poll 调用):只投信箱,任何阻塞都会拖垮 UI/取流。
 * worker 忙时新帧顶掉旧帧,旧帧立即归还——永远推理最新画面,不积压。 */
static void on_frame_push(const uint8_t *data, int w, int h, uint32_t frame_id)
{
    if (!s_ready || w <= 0 || h <= 0 || !data) {
        camera_nv12_release(frame_id);
        return;
    }

    uint32_t stale = 0;                 /* 被顶掉的旧帧,锁外归还 */
    pthread_mutex_lock(&s_mb_mtx);
    if (s_mb.has)
        stale = s_mb.fid;
    s_mb.data = data;
    s_mb.fid = frame_id;
    s_mb.w = w;
    s_mb.h = h;
    s_mb.has = true;
    pthread_cond_signal(&s_mb_cond);
    pthread_mutex_unlock(&s_mb_mtx);

    if (stale)
        camera_nv12_release(stale);
}

static void *vision_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_mb_mtx);
        while (!s_mb.has)
            pthread_cond_wait(&s_mb_cond, &s_mb_mtx);
        const uint8_t *data = s_mb.data;
        const uint32_t fid = s_mb.fid;
        const int w = s_mb.w, h = s_mb.h;
        s_mb.has = false;
        pthread_mutex_unlock(&s_mb_mtx);

        process_frame(data, w, h, fid); /* 所有路径内部保证归还缓冲 */
    }
    return NULL;                        /* 进程生命周期线程,无退出路径 */
}

static void process_frame(const uint8_t *data, int w, int h, uint32_t frame_id)
{
    /* IDLE/坏帧 → 立即归还(否则 4 缓冲耗尽,相机永久断流)。
     * (IDLE 可能在帧入箱后才发生——worker 这里再查一次,模式切换即时生效) */
    if (vision_service_get_mode() == DG_VMODE_IDLE || w <= 0 || h <= 0) {
        camera_nv12_release(frame_id);
        return;
    }

    /* 旋到预览同向:横置摄像头的原始帧里人是躺着的,检测模型只在正立脸的
     * 域内可靠(板上实测:喂原始帧时分数 0.999→0.5~0.7、关键点/框回归
     * 严重劣化、空场景 0.5x 幻检唤醒待机)。旋转后所有坐标与预览=屏幕同域 */
    const int rot = camera_rotation();
    const int rw = (rot == 90 || rot == 270) ? h : w;
    const int rh = (rot == 90 || rot == 270) ? w : h;
    const size_t need = (size_t)w * h * 3 / 2;   /* NV12;旋转前后字节数相同 */
    if (s_rot_cap < need) {
        uint8_t *nb = realloc(s_rot, need);
        if (!nb) {
            camera_nv12_release(frame_id);
            return;
        }
        s_rot = nb;
        s_rot_cap = need;
    }
    if (npu_pre_nv12_rotate(data, w, w, h, s_rot, rot) != DG_OK) {
        camera_nv12_release(frame_id);
        static int n_rot_err;
        if (n_rot_err++ < 3)
            DG_LOGE(TAG, "帧旋转失败,本帧跳过(共 %d)", n_rot_err);
        return;
    }

    if (rw != s_lb_src_w || rh != s_lb_src_h) {
        npu_letterbox_plan(rw, rh, s_in_w, s_in_h, &s_lb);
        s_lb_src_w = rw;
        s_lb_src_h = rh;
        DG_LOGI(TAG, "letterbox 计划 %dx%d(预览域)→ %dx%d(scale=%.4f,补边 %d,%d)",
                rw, rh, s_in_w, s_in_h, s_lb.scale, s_lb.pad_x, s_lb.pad_y);
    }

    if (npu_pre_nv12_letterbox_rgb(s_rot, rw, &s_lb, s_rgb) != DG_OK ||
        npu_model_run(s_face, s_rgb, s_in_bytes, DG_NPU_TYPE_U8) != DG_OK) {
        camera_nv12_release(frame_id);
        return;
    }
    uint32_t elems = 0;
    const bool ok_out =
        npu_model_output_f32(s_face, 0, s_loc, (uint32_t)s_nanchor * 4, &elems) == DG_OK &&
        npu_model_output_f32(s_face, 1, s_conf, (uint32_t)s_nanchor * 2, &elems) == DG_OK &&
        npu_model_output_f32(s_face, 2, s_landm, (uint32_t)s_nanchor * 10, &elems) == DG_OK;
    if (!ok_out) {
        camera_nv12_release(frame_id);
        DG_LOGE(TAG, "取输出失败(模型输出数与预期不符?)");
        return;
    }

    /* 检出阈值走配置(face.det_threshold):i8 量化后空场景会出现 0.5x 的
     * 幻检,0.5 的出厂线顶不住;板上用 2s 节流日志实测后调 */
    const float score_thresh = cfg_get()->face_det_threshold;
    int n = rknn_retinaface_decode(s_loc, s_conf, s_landm, s_nanchor, s_in_w,
                                   score_thresh, s_cand, RKNN_MAX_CAND);
    if (n < 0) {
        camera_nv12_release(frame_id);
        DG_LOGE(TAG, "解码失败(%d)", n);
        return;
    }
    if (n > RKNN_MAX_CAND)
        n = RKNN_MAX_CAND;
    n = rknn_nms(s_cand, n, RKNN_NMS_IOU);

    if (n <= 0) {
        camera_nv12_release(frame_id);
        /* 滞回:单帧漏检(分数抖动/识别帧占用造成的检测间隙)立刻发 LOST 会让
         * 脸框闪烁——600ms 内保持最后位置不发,超时才真正判定"人走了" */
        if (s_face_present && now_ms() - s_last_det_ms > 600)
            pub_face_lost();
        return;
    }
    s_last_det_ms = now_ms();

    /* 最大脸(检测模型空间)→ 预览/屏幕域坐标(框与关键点一并逆映射) */
    int best = 0;
    float best_area = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = (s_cand[i].x2 - s_cand[i].x1) * (s_cand[i].y2 - s_cand[i].y1);
        if (a > best_area) {
            best_area = a;
            best = i;
        }
    }
    rknn_face_t src = s_cand[best];
    for (int i = 0; i < RKNN_FACE_KPS; i++)
        npu_letterbox_unmap(&s_lb, s_cand[best].kps[i][0], s_cand[best].kps[i][1],
                            &src.kps[i][0], &src.kps[i][1]);
    npu_letterbox_unmap(&s_lb, s_cand[best].x1, s_cand[best].y1, &src.x1, &src.y1);
    npu_letterbox_unmap(&s_lb, s_cand[best].x2, s_cand[best].y2, &src.x2, &src.y2);
    if (src.x1 < 0) src.x1 = 0;
    if (src.y1 < 0) src.y1 = 0;
    if (src.x2 > (float)rw) src.x2 = (float)rw;
    if (src.y2 > (float)rh) src.y2 = (float)rh;

    /* ---- 识别(节流;要读旋转帧,须在归还缓冲前) ---- */
    const dg_vision_mode_t mode = vision_service_get_mode();
    const int64_t t = now_ms();
    if (s_rec_dim == RKNN_REC_DIM && mode != DG_VMODE_IDLE &&
        t - s_last_rec_ms >= RKNN_REC_MS) {
        s_last_rec_ms = t;
        float feat[RKNN_REC_DIM];
        if (recognize(s_rot, rw, rh, &src, s_cand[best].score, feat)) {
            /* 录入缓存:特征 + 同帧头像大图一把锁成对写(DETECT_ONLY 下
             * 也照常:录入页/拍摄页在该模式) */
            pthread_mutex_lock(&s_cap_mtx);
            memcpy(s_cap.data, feat, RKNN_FEATURE_BYTES);
            s_cap.len = RKNN_FEATURE_BYTES;
            s_cap.ms = t;
            memcpy(s_snap.data, s_avatar_warp, sizeof(s_snap.data));
            s_snap.ms = t;
            pthread_mutex_unlock(&s_cap_mtx);

            if (mode == DG_VMODE_VERIFY_11)
                verify_against_target(feat);
            else if (mode == DG_VMODE_DETECT_1N)
                search_1n(feat);
        }
    }

    /* 检测与识别都用完了,归还缓冲 */
    camera_nv12_release(frame_id);

    feed_liveness(&src, rw, rh);

    /* 检出分数节流日志(调 face.det_threshold 用) */
    {
        static int64_t last_log;
        static float last_score = -1.0f;
        if (t - last_log >= RKNN_BOX_LOG_MS && s_cand[best].score != last_score) {
            last_log = t;
            last_score = s_cand[best].score;
            DG_LOGI(TAG, "检出 %d 张脸,最大脸分数 %.3f(阈值 %.2f,%.0fx%.0f)",
                    n, s_cand[best].score, score_thresh,
                    src.x2 - src.x1, src.y2 - src.y1);
        }
    }

    /* 脸框事件节流(15Hz)。坐标已是屏幕像素,恒等发布 */
    {
        static int64_t last_pub;
        if (t - last_pub < RKNN_PUB_MS)
            return;
        last_pub = t;
    }
    ev_face_box_t box;
    memset(&box, 0, sizeof(box));
    box.state = DG_BOX_DETECTED;
    box.x = (int32_t)src.x1;
    box.y = (int32_t)src.y1;
    box.w = (int32_t)(src.x2 - src.x1);
    box.h = (int32_t)(src.y2 - src.y1);
    s_face_present = true;
    EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
}

/* 录入抓取请求:提交近 3s 缓存特征 + 同帧头像(编码成 JPEG 走照片槽) */
static int on_capture_req(const event_t *e, void *ud)
{
    (void)ud;
    const ev_capture_req_t *r = (const ev_capture_req_t *)e->data;

    /* 大缓冲一律 static:总线分发线程栈仅 64KB(EVENT_BUS_TASK_STACK_SIZE),
     * 局部数组会栈溢出;总线单线程分发,静态缓冲无重入 */
    static uint8_t buf[DG_FEATURE_MAX];
    static uint8_t snap[RKNN_AVATAR_SZ * RKNN_AVATAR_SZ * 3];
    static uint8_t jpeg[DG_AVATAR_JPEG_MAX];
    bool has_snap = false;

    pthread_mutex_lock(&s_cap_mtx);
    const int64_t age = now_ms() - s_cap.ms;
    uint16_t len = s_cap.len;
    if (len && age <= 3000) {
        memcpy(buf, s_cap.data, len);
        /* 同帧校验:ms 相同才收下(一把锁下成对写,此处必成立,防御将来
         * 拆锁改动) */
        if (s_snap.ms == s_cap.ms) {
            memcpy(snap, s_snap.data, sizeof(snap));
            has_snap = true;
        }
    } else {
        len = 0;
    }
    pthread_mutex_unlock(&s_cap_mtx);

    if (!len) {
        DG_LOGW(TAG, "录入取特征:近 3s 无合格人脸(请正对镜头重试)");
        return 0;
    }
    DG_LOGI(TAG, "录入取特征:%u B(滞后 %lldms)→ %s", len, (long long)age,
            r->user_id);

    /* 头像:此刻编码一次(几毫秒,总线线程可承受;不是每帧)。
     * 先于 submit_feature 入槽——submit 发布的事件若同步分发,enroll
     * 立刻按同一 seq 取件,顺序不能反 */
    if (has_snap) {
        size_t jlen = 0;
        const int rc = dg_jpeg_encode_rgb(snap, RKNN_AVATAR_SZ, RKNN_AVATAR_SZ,
                                          80, jpeg, sizeof(jpeg), &jlen);
        if (rc == DG_OK)
            vision_service_put_avatar(r->seq, jpeg, jlen);
        else
            DG_LOGW(TAG, "头像编码失败(%d):本次入库无头像", rc);
    } else {
        DG_LOGW(TAG, "同帧头像缺失:本次入库无头像");
    }
    vision_service_submit_feature(r->user_id, r->seq, buf, len);
    return 0;
}

/* 1:1 目标特征装载(模式钩子,总线线程调用) */
static const char *s_target_want;

static int target_iter_cb(const char *user_id, const uint8_t *plain,
                          uint16_t len, void *ud)
{
    (void)ud;
    if (!s_target_want || strcmp(user_id, s_target_want) != 0)
        return 0;
    if (len != RKNN_FEATURE_BYTES)
        return 1;                           /* 命中但长度不符,中止 */
    pthread_mutex_lock(&s_target.mtx);
    memcpy(s_target.data, plain, len);
    s_target.len = len;
    snprintf(s_target.uid, sizeof(s_target.uid), "%s", user_id);
    pthread_mutex_unlock(&s_target.mtx);
    return 1;
}

static void on_mode_changed(dg_vision_mode_t mode, const char *user_id)
{
    pthread_mutex_lock(&s_target.mtx);
    s_target.len = 0;
    s_target.uid[0] = '\0';
    pthread_mutex_unlock(&s_target.mtx);

    if (mode == DG_VMODE_IDLE) {
        EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
        s_face_present = false;
    }
    if (mode != DG_VMODE_VERIFY_11 || !user_id || !user_id[0])
        return;
    s_target_want = user_id;
    const int rc = db_user_iter_face(target_iter_cb, NULL);
    s_target_want = NULL;
    pthread_mutex_lock(&s_target.mtx);
    const uint16_t tlen = s_target.len;
    pthread_mutex_unlock(&s_target.mtx);
    if (rc != DG_OK || tlen != RKNN_FEATURE_BYTES)
        DG_LOGW(TAG, "1:1 目标 %s 无可用人脸特征(等 FSM 5s 超时)", user_id);
    else
        DG_LOGI(TAG, "1:1 目标特征已装载 %s", user_id);
}

static int rknn_start(bool enable_mock)
{
    (void)enable_mock;

    const char *dir = env_or("DG_RKNN_MODEL_DIR", RKNN_DEFAULT_DIR);
    char fpath[512], rpath[512];
    snprintf(fpath, sizeof(fpath), "%s/%s", dir,
             env_or("DG_RKNN_FACE_MODEL", RKNN_FACE_MODEL));
    snprintf(rpath, sizeof(rpath), "%s/%s", dir,
             env_or("DG_RKNN_REC_MODEL", RKNN_REC_MODEL));

    DG_LOGI(TAG, "rknn 后端启动:运行时 %s", npu_hal_version());

    /* ---- 检测模型 ---- */
    s_face = npu_model_load(fpath);
    if (!s_face)
        return DG_ERR_IO;
    npu_attr_t in;
    if (npu_model_input_attr(s_face, 0, &in) != DG_OK || npu_model_output_num(s_face) != 3) {
        DG_LOGE(TAG, "检测模型规格异常(输出应为 3)");
        goto fail;
    }
    s_in_w = in.width;
    s_in_h = in.height;
    s_in_bytes = in.nbytes;
    npu_attr_t o0;
    if (npu_model_output_attr(s_face, 0, &o0) != DG_OK) {
        DG_LOGE(TAG, "查检测输出属性失败");
        goto fail;
    }
    s_nanchor = (o0.n_dims >= 2) ? (int)o0.dims[1] : 0;
    if (s_nanchor != rknn_retinaface_anchor_count(s_in_w)) {
        DG_LOGE(TAG, "锚框数 %d 与 %d 输入期望的 %d 不符——模型与解码器不匹配",
                s_nanchor, s_in_w, rknn_retinaface_anchor_count(s_in_w));
        goto fail;
    }

    /* ---- 识别模型 ---- */
    s_rec = npu_model_load(rpath);
    if (!s_rec) {
        DG_LOGE(TAG, "识别模型加载失败:%s(检测不可用识别)", rpath);
        goto fail;
    }
    npu_attr_t rin;
    if (npu_model_input_attr(s_rec, 0, &rin) != DG_OK) {
        DG_LOGE(TAG, "查识别输入属性失败");
        goto fail;
    }
    s_rec_in_bytes = (size_t)rin.elems * 4;     /* F32 归一化输入 */
    npu_attr_t rout;
    if (npu_model_output_attr(s_rec, 0, &rout) != DG_OK || rout.elems != RKNN_REC_DIM) {
        DG_LOGE(TAG, "识别输出 %u 维,期望 %d——不是 512 维 ArcFace?",
                rout.elems, RKNN_REC_DIM);
        goto fail;
    }
    s_rec_dim = (int)rout.elems;

    /* ---- 缓冲 ---- */
    s_rgb      = malloc((size_t)s_in_w * s_in_h * 3);
    s_loc      = malloc(sizeof(float) * (size_t)s_nanchor * 4);
    s_conf     = malloc(sizeof(float) * (size_t)s_nanchor * 2);
    s_landm    = malloc(sizeof(float) * (size_t)s_nanchor * 10);
    s_cand     = malloc(sizeof(rknn_face_t) * RKNN_MAX_CAND);
    s_roi      = malloc((size_t)RKNN_ROI_MAX * RKNN_ROI_MAX * 3);
    s_aligned  = malloc((size_t)112 * 112 * 3);
    s_norm_in  = malloc(s_rec_in_bytes);
    if (!s_rgb || !s_loc || !s_conf || !s_landm || !s_cand ||
        !s_roi || !s_aligned || !s_norm_in) {
        DG_LOGE(TAG, "缓冲分配失败");
        goto fail;
    }

    /* ---- 接线与特征库装载 ---- */
    camera_set_nv12_listener(on_frame_push, NULL);
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req, NULL);
    db_user_iter_face(lib_load_cb, NULL);
    pthread_mutex_lock(&s_lib.mtx);
    const int loaded = s_lib.n;
    pthread_mutex_unlock(&s_lib.mtx);

    /* worker 线程最后起:s_ready 就位后才有帧可处理(进程生命周期,不 join) */
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, vision_worker, NULL) != 0) {
            DG_LOGE(TAG, "推理 worker 线程创建失败");
            goto fail;
        }
        pthread_detach(tid);
    }

    s_ready = true;
    DG_LOGI(TAG, "rknn 就绪:检测 %s %dx%d(锚框 %d,阈值 %.2f)+ 识别 %s(%d 维,"
                 "特征 %d B),库 %d 人",
            RKNN_FACE_MODEL, s_in_w, s_in_h, s_nanchor,
            cfg_get()->face_det_threshold,
            RKNN_REC_MODEL, s_rec_dim, RKNN_FEATURE_BYTES, loaded);
    return DG_OK;

fail:
    if (s_face) { npu_model_release(s_face); s_face = NULL; }
    if (s_rec)  { npu_model_release(s_rec);  s_rec = NULL; }
    free(s_rgb);     s_rgb = NULL;
    free(s_rot);     s_rot = NULL; s_rot_cap = 0;
    free(s_loc);     s_loc = NULL;
    free(s_conf);    s_conf = NULL;
    free(s_landm);   s_landm = NULL;
    free(s_cand);    s_cand = NULL;
    free(s_roi);     s_roi = NULL;
    free(s_aligned); s_aligned = NULL;
    free(s_norm_in); s_norm_in = NULL;
    return DG_ERR_IO;
}

/* 后端注册项(装配层 app/main.c 注册;契约见 vision_backend.h) */
const vision_backend_ops_t vision_backend_rknn = {
    .name = "rknn",
    .model_tag = "rknn-arcface-r50-v1",  /* ArcFace-R50 512 维特征空间 */
    .has_landmarks = true,               /* 5 点关键点,供 B8 几何活体 */
    .start = rknn_start,
    .lib_add = lib_add,
    .lib_del = lib_del,
    .compare = rknn_cmp,
    .on_mode = on_mode_changed,
};
