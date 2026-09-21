/*
 * face_quality.c — 人脸质量闸门实现(纯 C,零依赖)
 *
 * 清晰度用 4 邻域 Laplacian 的方差:这是"焦点/运动模糊"最经典也最便宜的指标
 * (对焦清晰的图像边缘二阶导大、方差大;模糊则相反)。之所以用方差而不是
 * 均值:均值会被大面积平坦皮肤区拉平,方差才对边缘敏感。
 *
 * 成本:112×112 灰度上约 3.7 万次整数运算,板上 A53 亚毫秒级,可以每帧算。
 */
#include "face_quality.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

void face_quality_gray(const uint8_t *rgb, int w, int h, uint8_t *gray)
{
    if (!rgb || !gray || w <= 0 || h <= 0)
        return;
    const int n = w * h;
    for (int i = 0; i < n; i++) {
        const uint8_t r = rgb[i * 3];
        const uint8_t g = rgb[i * 3 + 1];
        const uint8_t b = rgb[i * 3 + 2];
        /* 整数近似(权重和 256):避免浮点,亮度误差 <1/255,判模糊足够 */
        gray[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    }
}

double face_quality_blur(const uint8_t *gray, int w, int h)
{
    if (!gray || w < 3 || h < 3)
        return 0.0;

    /* 内点(有 4 邻域)的 Laplacian: 4*c - 上 - 下 - 左 - 右 */
    double sum = 0.0, sum2 = 0.0;
    int n = 0;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            const int i = y * w + x;
            const int lap = 4 * gray[i] - gray[i - 1] - gray[i + 1]
                            - gray[i - w] - gray[i + w];
            sum += lap;
            sum2 += (double)lap * lap;
            n++;
        }
    }
    if (n == 0)
        return 0.0;
    const double mean = sum / n;
    const double var = sum2 / n - mean * mean;
    return var > 0.0 ? var : 0.0;       /* 浮点误差可能给极小负数 */
}

face_quality_verdict_t face_quality_check(const face_quality_t *q,
                                          const face_quality_thr_t *thr)
{
    if (!q || !thr)
        return FQ_ERR_PARAM;

    /* 阈值 <=0 = 该项未启用(板上标定期可以先只开清晰度) */
    if (thr->min_face_px > 0 && q->face_px < thr->min_face_px)
        return FQ_ERR_SMALL;
    if (thr->blur_min > 0 && q->blur < thr->blur_min)
        return FQ_ERR_BLURRY;
    if (thr->det_score_min > 0 && q->det_score < thr->det_score_min)
        return FQ_ERR_LOW_SCORE;
    return FQ_OK;
}

const char *face_quality_reason_str(face_quality_verdict_t v)
{
    switch (v) {
    case FQ_OK:            return "ok";
    case FQ_ERR_SMALL:     return "face too small";
    case FQ_ERR_BLURRY:    return "too blurry";
    case FQ_ERR_LOW_SCORE: return "low detect score";
    default:               return "bad param";
    }
}
