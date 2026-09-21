/*
 * test_face_quality.c — 人脸质量闸门单元测试(宿主,无需板子/NPU)
 *
 * 清晰度指标最容易"写了个能跑但测不出东西的东西":所以这里不测实现细节,
 * 而测**它是否真能区分清晰与模糊**——用合成图:
 *   棋盘/文字边缘(清晰)vs 同一图做均值模糊(模糊),断言前者方差显著更大。
 * 这是这个指标存在的唯一理由,测住它才算数。
 */
#include "dg_test.h"
#include "face_quality.h"

#include <string.h>
#include <math.h>

/* ---- 清晰度:必须区分"锐"与"糊" -------------------------------------- */

static void make_checker(uint8_t *gray, int w, int h, int cell)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            gray[y * w + x] = (((x / cell) + (y / cell)) & 1) ? 230 : 25;
}

static void box_blur(const uint8_t *src, uint8_t *dst, int w, int h, int radius)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sum = 0, n = 0;
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                        continue;
                    sum += src[yy * w + xx];
                    n++;
                }
            }
            dst[y * w + x] = (uint8_t)(sum / n);
        }
    }
}

static void test_blur_metric_discriminates(void)
{
    enum { W = 112, H = 112 };
    static uint8_t sharp[W * H], blurred[W * H], tmp[W * H];

    make_checker(sharp, W, H, 4);                 /* 4 像素细格 = 强边缘 */
    box_blur(sharp, tmp, W, H, 2);
    box_blur(tmp, blurred, W, H, 2);              /* 两次 5×5 均值 = 明显模糊 */

    const double b_sharp = face_quality_blur(sharp, W, H);
    const double b_blur  = face_quality_blur(blurred, W, H);

    printf("  清晰=%.1f  模糊=%.1f\n", b_sharp, b_blur);
    DG_CHECK(b_sharp > 1000.0);                   /* 棋盘图方差应当很大 */
    DG_CHECK(b_blur < b_sharp * 0.1);             /* 模糊后退化一个数量级以上 */
}

/* 全平图(纯色/纯灰)方差为 0 —— 不是"极清晰" */
static void test_blur_flat_is_zero(void)
{
    static uint8_t flat[64 * 64];
    memset(flat, 128, sizeof(flat));
    DG_CHECK(face_quality_blur(flat, 64, 64) < 1e-6);
}

/* 边界与非法入参不崩、不产生 NaN */
static void test_blur_edges(void)
{
    static uint8_t g[4 * 4];
    memset(g, 10, sizeof(g));
    DG_CHECK(face_quality_blur(NULL, 8, 8) == 0.0);
    DG_CHECK(face_quality_blur(g, 2, 2) == 0.0);   /* 无内点 */
    DG_CHECK(face_quality_blur(g, 0, 0) == 0.0);
    const double v = face_quality_blur(g, 4, 4);
    DG_CHECK(v >= 0.0 && !isnan(v));
}

/* ---- 灰度转换 ---------------------------------------------------------- */

static void test_gray(void)
{
    /* 纯红/纯绿/纯蓝/白:按 0.299/0.587/0.114 近似 */
    const uint8_t rgb[4 * 3] = { 255,0,0,  0,255,0,  0,0,255,  255,255,255 };
    uint8_t g[4];
    face_quality_gray(rgb, 4, 1, g);
    DG_CHECK(g[0] > 70 && g[0] < 82);             /* 红 ≈ 76 */
    DG_CHECK(g[1] > 145 && g[1] < 155);           /* 绿 ≈ 150 */
    DG_CHECK(g[2] > 25 && g[2] < 34);             /* 蓝 ≈ 29 */
    DG_CHECK(g[3] == 255);                        /* 白 */
    face_quality_gray(NULL, 4, 1, g);             /* NULL 安全 */
    DG_CHECK(1);
}

/* ---- 判定:逐项 + 未启用项(阈值 0)------------------------------------ */

static void test_check(void)
{
    const face_quality_thr_t thr = { .min_face_px = 80, .blur_min = 50.0,
                                     .det_score_min = 0.70 };

    face_quality_t q = { .face_px = 120, .blur = 300.0, .det_score = 0.95 };
    DG_CHECK(face_quality_check(&q, &thr) == FQ_OK);

    q.face_px = 60;                                /* 太小 */
    DG_CHECK(face_quality_check(&q, &thr) == FQ_ERR_SMALL);
    q.face_px = 120;

    q.blur = 10.0;                                 /* 太糊 */
    DG_CHECK(face_quality_check(&q, &thr) == FQ_ERR_BLURRY);
    q.blur = 300.0;

    q.det_score = 0.40;                            /* 置信度低 */
    DG_CHECK(face_quality_check(&q, &thr) == FQ_ERR_LOW_SCORE);

    /* 阈值 0 = 不启用:低分也放行(板上标定期只开清晰度的用法) */
    const face_quality_thr_t only_blur = { .min_face_px = 0, .blur_min = 50.0,
                                           .det_score_min = 0 };
    DG_CHECK(face_quality_check(&q, &only_blur) == FQ_OK);

    /* 非法入参 */
    DG_CHECK(face_quality_check(NULL, &thr) == FQ_ERR_PARAM);
    DG_CHECK(face_quality_check(&q, NULL) == FQ_ERR_PARAM);
}

int main(void)
{
    test_blur_metric_discriminates();
    test_blur_flat_is_zero();
    test_blur_edges();
    test_gray();
    test_check();
    DG_TEST_EXIT();
}
