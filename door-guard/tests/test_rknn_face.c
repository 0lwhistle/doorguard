/*
 * test_rknn_face.c — 自组 rknn 人脸后处理单元测试(宿主,无需板子/NPU)
 *
 * 覆盖:SCRFD 解码(anchor 编号与 stride 还原)、截断上报、IoU、NMS、
 * 5 点对齐(参考点自映射=单位阵、缩放平移、warp 采样)、特征余弦/归一化。
 *
 * 这些是"能算错但看不出来"的部分:解码错一格、stride 用错、对齐方向反了,
 * 上板只会看到"框歪了/识别不过",很难定位。所以在这里用合成数据钉死。
 */
#include "dg_test.h"
#include "rknn_face.h"

#include <math.h>
#include <string.h>

#define EPS 1e-4f

static int feq(float a, float b) { return fabsf(a - b) < 1e-3f; }

/* ---- SCRFD 解码:anchor 编号 + stride 还原 ------------------------------- */

static void test_decode(void)
{
    /* 2×2 网格 × 1 anchor,stride 8:anchor idx=(y*gw+x)*na+a */
    float score[4] = { 0.0f, 0.9f, 0.0f, 0.0f };      /* 只有 idx=1(x=1,y=0)*/
    float bbox[16] = { 0 };
    float kps[40] = { 0 };
    /* idx=1 的框:距 anchor 中心四个方向都是 1×stride */
    bbox[4] = 1.0f; bbox[5] = 1.0f; bbox[6] = 1.0f; bbox[7] = 1.0f;
    /* idx=1 的 5 点:x 依次 0,1,2,3,4(×stride),y 全 0 */
    for (int j = 0; j < RKNN_FACE_KPS; j++)
        kps[10 + j * 2] = (float)j;

    rknn_scrfd_level_t lv = { .score = score, .bbox = bbox, .kps = kps,
                              .grid_w = 2, .grid_h = 2, .n_anchors = 1, .stride = 8 };

    rknn_face_t out[8];
    int n = rknn_scrfd_decode(&lv, 1, 0.5f, out, 8);
    DG_CHECK(n == 1);

    /* anchor 中心 = (1*8, 0*8) = (8,0);框 = 中心 ∓ 1*stride */
    DG_CHECK(feq(out[0].x1, 0.0f) && feq(out[0].y1, -8.0f));
    DG_CHECK(feq(out[0].x2, 16.0f) && feq(out[0].y2, 8.0f));
    DG_CHECK(feq(out[0].score, 0.9f));
    /* 关键点 = 中心 + k*stride */
    DG_CHECK(feq(out[0].kps[0][0], 8.0f) && feq(out[0].kps[0][1], 0.0f));
    DG_CHECK(feq(out[0].kps[4][0], 40.0f) && feq(out[0].kps[4][1], 0.0f));
}

static void test_decode_filter_and_truncate(void)
{
    float score[4] = { 0.6f, 0.7f, 0.2f, 0.9f };      /* 0.2 不过阈 */
    float bbox[16] = { 0 }, kps[40] = { 0 };
    rknn_scrfd_level_t lv = { .score = score, .bbox = bbox, .kps = kps,
                              .grid_w = 2, .grid_h = 2, .n_anchors = 1, .stride = 8 };

    rknn_face_t out[8];
    DG_CHECK(rknn_scrfd_decode(&lv, 1, 0.5f, out, 8) == 3);   /* 3 个过阈 */

    /* 容量 1:仍返回 3(调用方知道被截断),只写 1 个 */
    rknn_face_t one[1];
    DG_CHECK(rknn_scrfd_decode(&lv, 1, 0.5f, one, 1) == 3);

    /* 非法参数 */
    DG_CHECK(rknn_scrfd_decode(NULL, 1, 0.5f, out, 8) < 0);
    DG_CHECK(rknn_scrfd_decode(&lv, 0, 0.5f, out, 8) < 0);
}

/* 3 档 stride 齐全时:各档命中都能解出来(模拟 SCRFD 的 9 输出布局) */
static void test_decode_multilevel(void)
{
    float s8[4] = { 0.8f, 0, 0, 0 }, b8[16] = { 0 }, k8[40] = { 0 };
    float s16[1] = { 0.7f },           b16[4] = { 0 }, k16[10] = { 0 };

    rknn_scrfd_level_t lv[2] = {
        { .score = s8,  .bbox = b8,  .kps = k8,  .grid_w = 2, .grid_h = 2,
          .n_anchors = 1, .stride = 8 },
        { .score = s16, .bbox = b16, .kps = k16, .grid_w = 1, .grid_h = 1,
          .n_anchors = 1, .stride = 16 },
    };
    rknn_face_t out[8];
    DG_CHECK(rknn_scrfd_decode(lv, 2, 0.5f, out, 8) == 2);
}

/* ---- IoU / NMS ---------------------------------------------------------- */

static void test_iou(void)
{
    rknn_face_t a = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10, .score = 1 };
    rknn_face_t b = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10, .score = 1 };
    DG_CHECK(feq(rknn_iou(&a, &b), 1.0f));

    rknn_face_t far = { .x1 = 100, .y1 = 100, .x2 = 110, .y2 = 110, .score = 1 };
    DG_CHECK(feq(rknn_iou(&a, &far), 0.0f));

    /* 半重叠:交 50,并 150 → 1/3 */
    rknn_face_t half = { .x1 = 5, .y1 = 0, .x2 = 15, .y2 = 10, .score = 1 };
    DG_CHECK(feq(rknn_iou(&a, &half), 1.0f / 3.0f));
}

static void test_nms(void)
{
    rknn_face_t f[3] = {
        { .x1 = 0,   .y1 = 0,   .x2 = 10,   .y2 = 10,   .score = 0.9f },
        { .x1 = 1,   .y1 = 1,   .x2 = 11,   .y2 = 11,   .score = 0.8f },  /* 与首个 IoU 0.68 */
        { .x1 = 100, .y1 = 100, .x2 = 110,  .y2 = 110,  .score = 0.7f },
    };
    int keep = rknn_nms(f, 3, 0.45f);
    DG_CHECK(keep == 2);
    DG_CHECK(feq(f[0].score, 0.9f));     /* 降序:最高分在前 */
    DG_CHECK(feq(f[1].score, 0.7f));     /* 被抑制的 0.8 已移除,远端框留下 */

    /* 空/单元素边界 */
    DG_CHECK(rknn_nms(f, 0, 0.45f) == 0);
    DG_CHECK(rknn_nms(f, 1, 0.45f) == 1);
}

/* ---- 5 点对齐 ----------------------------------------------------------- */

/* 参考点自映射 = 单位阵(最有力的自检:变换的定点就是它自己) */
static const float k_ref[RKNN_FACE_KPS][2] = {
    { 38.2946f, 51.6963f }, { 73.5318f, 51.5014f }, { 56.0252f, 71.7366f },
    { 41.5493f, 92.3655f }, { 70.7299f, 92.2041f },
};

static void test_align_identity(void)
{
    float m[6];
    DG_CHECK(rknn_align_plan(k_ref, m) == 0);
    DG_CHECK(feq(m[0], 1.0f) && feq(m[1], 0.0f) && feq(m[2], 0.0f));
    DG_CHECK(feq(m[3], 0.0f) && feq(m[4], 1.0f) && feq(m[5], 0.0f));
}

static void test_align_scale_translate(void)
{
    /* src = 2×参考 + (10,20) → 变换应把它还原回参考:x'=(x-10)/2 */
    float src[RKNN_FACE_KPS][2];
    for (int i = 0; i < RKNN_FACE_KPS; i++) {
        src[i][0] = k_ref[i][0] * 2.0f + 10.0f;
        src[i][1] = k_ref[i][1] * 2.0f + 20.0f;
    }
    float m[6];
    DG_CHECK(rknn_align_plan(src, m) == 0);
    DG_CHECK(feq(m[0], 0.5f));           /* a  */
    DG_CHECK(feq(m[4], 0.5f));           /* a  */
    DG_CHECK(feq(m[2], -5.0f));          /* tx */
    DG_CHECK(feq(m[5], -10.0f));         /* ty */

    /* 正向验证一个点:左眼应落回参考左眼 */
    const float x = src[0][0], y = src[0][1];
    DG_CHECK(feq(m[0] * x + m[1] * y + m[2], k_ref[0][0]));
    DG_CHECK(feq(m[3] * x + m[4] * y + m[5], k_ref[0][1]));
}

static void test_align_warp_identity(void)
{
    uint8_t src[8 * 8 * 3];
    for (int i = 0; i < 8 * 8 * 3; i++)
        src[i] = (uint8_t)(i % 251);
    uint8_t dst[4 * 4 * 3];
    memset(dst, 0xAA, sizeof(dst));

    const float I[6] = { 1, 0, 0, 0, 1, 0 };
    rknn_align_warp(src, 8, 8, I, dst, 4, 4);

    int same = 1;
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 4; u++)
            for (int c = 0; c < 3; c++)
                if (dst[(v * 4 + u) * 3 + c] != src[(v * 8 + u) * 3 + c])
                    same = 0;
    DG_CHECK(same);
}

static void test_align_warp_oob_zero(void)
{
    /* 平移出界:全填 0,不是脏内存 */
    uint8_t src[4 * 4 * 3];
    memset(src, 0xFF, sizeof(src));
    uint8_t dst[2 * 2 * 3];
    memset(dst, 0x11, sizeof(dst));
    const float M[6] = { 1, 0, 100.0f, 0, 1, 100.0f };   /* 源坐标 = 目标-100 → 负 */
    rknn_align_warp(src, 4, 4, M, dst, 2, 2);
    int allzero = 1;
    for (int i = 0; i < 2 * 2 * 3; i++)
        if (dst[i] != 0)
            allzero = 0;
    DG_CHECK(allzero);
}

/* ---- 特征 --------------------------------------------------------------- */

static void test_feature(void)
{
    float a[4] = { 1, 2, 3, 4 };
    float b[4] = { 2, 4, 6, 8 };
    DG_CHECK(feq(rknn_cosine(a, b, 4), 1.0f));       /* 同向 */

    float c[4] = { 2, -1, 0, 0 };                    /* 与 a 正交(a·c=0)*/
    DG_CHECK(feq(rknn_cosine(a, c, 4), 0.0f));

    rknn_l2_normalize(a, 4);
    float norm = 0;
    for (int i = 0; i < 4; i++)
        norm += a[i] * a[i];
    DG_CHECK(feq(sqrtf(norm), 1.0f));

    float zero[3] = { 0, 0, 0 };
    rknn_l2_normalize(zero, 3);                     /* 零向量不动,不产生 NaN */
    DG_CHECK(zero[0] == 0.0f && zero[1] == 0.0f && zero[2] == 0.0f);
}

/* ---- RetinaFace 解码 ----------------------------------------------------
 * 用 64 输入的缩样(size=64 → 8²×2 + 4²×2 + 2²×2 = 168 个锚框),
 * 便于逐点核对:锚框 0 = (step 8, i=0, j=0, min_size 16)
 *   pw = 16/64 = 0.25, 中心 = (0.5*8/64) = 0.0625
 *   → loc=0 时框 = ((0.0625-0.125)*64, …) = (-4,-4,12,12)
 */
#define RF_N 168

static float rf_loc[RF_N * 4];
static float rf_conf[RF_N * 2];
static float rf_landm[RF_N * 10];

static void rf_reset(void)
{
    memset(rf_loc, 0, sizeof(rf_loc));
    memset(rf_conf, 0, sizeof(rf_conf));
    memset(rf_landm, 0, sizeof(rf_landm));
}

static void test_retinaface_anchor_count(void)
{
    DG_CHECK(rknn_retinaface_anchor_count(64) == RF_N);
    DG_CHECK(rknn_retinaface_anchor_count(320) == 4200);   /* 与板上实测一致 */
}

static void test_retinaface_decode_prior(void)
{
    rf_reset();
    rf_conf[1] = 0.9f;                      /* 锚框 0 的人脸分(索引 1 = face 类) */

    rknn_face_t out[8];
    int n = rknn_retinaface_decode(rf_loc, rf_conf, rf_landm, RF_N, 64, 0.5f, out, 8);
    DG_CHECK(n == 1);
    /* loc=0 → 框就是锚框本身;关键点在锚框中心 */
    DG_CHECK(feq(out[0].x1, -4.0f) && feq(out[0].y1, -4.0f));
    DG_CHECK(feq(out[0].x2, 12.0f) && feq(out[0].y2, 12.0f));
    DG_CHECK(feq(out[0].score, 0.9f));
    DG_CHECK(feq(out[0].kps[0][0], 4.0f) && feq(out[0].kps[0][1], 4.0f));
}

static void test_retinaface_decode_offset(void)
{
    rf_reset();
    rf_conf[1] = 0.9f;
    rf_loc[0] = 1.0f;                       /* cx += 1*0.1*0.25 = 0.025 */
    rknn_face_t out[8];
    DG_CHECK(rknn_retinaface_decode(rf_loc, rf_conf, rf_landm, RF_N, 64, 0.5f, out, 8) == 1);
    /* x1 = (0.0625+0.025-0.125)*64 = -2.4;宽不变 */
    DG_CHECK(feq(out[0].x1, -2.4f));
    DG_CHECK(feq(out[0].x2, 13.6f));
}

static void test_retinaface_decode_badcount(void)
{
    rf_reset();
    rf_conf[1] = 0.9f;
    rknn_face_t out[8];
    /* 锚框数不符 = 解码方案与模型不是一套 → -2,而不是给一堆错框 */
    DG_CHECK(rknn_retinaface_decode(rf_loc, rf_conf, rf_landm, RF_N - 1, 64, 0.5f, out, 8) == -2);
    /* size 非法属于参数错(-1),与"锚框数不符"区分开 */
    DG_CHECK(rknn_retinaface_decode(rf_loc, rf_conf, rf_landm, RF_N, 0, 0.5f, out, 8) == -1);
}

static void test_retinaface_decode_threshold(void)
{
    rf_reset();                             /* 全 0 分 → 一个都不出 */
    rknn_face_t out[8];
    DG_CHECK(rknn_retinaface_decode(rf_loc, rf_conf, rf_landm, RF_N, 64, 0.5f, out, 8) == 0);

    /* 用了 [.,1](face 类)而不是 [.,0]:只在 [.,0] 上给高分应当不命中 */
    rf_conf[0] = 0.99f;
    DG_CHECK(rknn_retinaface_decode(rf_loc, rf_conf, rf_landm, RF_N, 64, 0.5f, out, 8) == 0);
}

int main(void)
{
    test_decode();
    test_decode_filter_and_truncate();
    test_decode_multilevel();
    test_retinaface_anchor_count();
    test_retinaface_decode_prior();
    test_retinaface_decode_offset();
    test_retinaface_decode_badcount();
    test_retinaface_decode_threshold();
    test_iou();
    test_nms();
    test_align_identity();
    test_align_scale_translate();
    test_align_warp_identity();
    test_align_warp_oob_zero();
    test_feature();
    DG_TEST_EXIT();
}
