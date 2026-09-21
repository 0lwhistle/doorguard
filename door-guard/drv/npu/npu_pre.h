/*
 * npu_pre.h — 送 NPU 前的图像预处理(RGA letterbox)
 *
 * 检测模型要的是**方形**输入,而相机是 16:9。直接拉伸会让脸变扁、影响检出与
 * 关键点位置,所以走 letterbox:等比缩放后居中补边(补 114,InsightFace 约定)。
 *
 * 拆两层:
 *   - **坐标数学**(本头文件的 static inline):算 letterbox 计划、把模型输入
 *     坐标逆映射回源图坐标。纯算术,宿主可测(tests/test_npu_pre.c)。
 *   - **RGA 硬件调用**(npu_pre.c):NV12 → RGB888 缩放+补边,只在交叉编译里编
 *     (宿主无 librga)。板上要验的只是"硬件路径接得对不对",数学不必陪跑。
 */
#ifndef DG_NPU_PRE_H
#define DG_NPU_PRE_H

#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** letterbox 补边值(与 rknn_model_zoo 的 RetinaFace 例程一致) */
#define NPU_PRE_PAD_VALUE 114

/** letterbox 变换参数:源(相机)↔ 模型输入 的换算依据 */
typedef struct {
    int   src_w, src_h;   /**< 源 NV12 尺寸 */
    int   dst_w, dst_h;   /**< 模型输入尺寸(通常方形,如 320×320) */
    float scale;          /**< 等比缩放系数 = min(dst_w/src_w, dst_h/src_h) */
    int   pad_x, pad_y;   /**< 居中补边(左/上偏移) */
    int   fit_w, fit_h;   /**< 缩放后的有效区域尺寸 */
} npu_letterbox_t;

/** 算 letterbox 计划(纯数学;参数非法时清零返回) */
static inline void npu_letterbox_plan(int src_w, int src_h, int dst_w, int dst_h,
                                      npu_letterbox_t *lb)
{
    if (!lb)
        return;
    lb->src_w = src_w;
    lb->src_h = src_h;
    lb->dst_w = dst_w;
    lb->dst_h = dst_h;
    lb->scale = 0.0f;
    lb->pad_x = lb->pad_y = 0;
    lb->fit_w = lb->fit_h = 0;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    const float sx = (float)dst_w / (float)src_w;
    const float sy = (float)dst_h / (float)src_h;
    lb->scale = sx < sy ? sx : sy;
    lb->fit_w = (int)((float)src_w * lb->scale + 0.5f);
    lb->fit_h = (int)((float)src_h * lb->scale + 0.5f);
    /* 有效区不能超画布(浮点取整可能多 1 像素) */
    if (lb->fit_w > dst_w) lb->fit_w = dst_w;
    if (lb->fit_h > dst_h) lb->fit_h = dst_h;
    lb->pad_x = (dst_w - lb->fit_w) / 2;
    lb->pad_y = (dst_h - lb->fit_h) / 2;
}

/** 模型输入坐标 → 源图坐标(解码出的框要画到相机图上,必须逆映射) */
static inline void npu_letterbox_unmap(const npu_letterbox_t *lb,
                                       float x, float y, float *ox, float *oy)
{
    if (!lb || lb->scale <= 0.0f) {
        if (ox) *ox = x;
        if (oy) *oy = y;
        return;
    }
    if (ox) *ox = (x - (float)lb->pad_x) / lb->scale;
    if (oy) *oy = (y - (float)lb->pad_y) / lb->scale;
}

/**
 * RGA:NV12 → letterbox 后的 RGB888(dst_w×dst_h×3 字节)。
 * 先整画布补 NPU_PRE_PAD_VALUE,再把等比缩放后的图像画进居中区域(一次 improcess)。
 * @param nv12   NV12 数据首地址(Y 平面起;UV 紧随其后)
 * @param stride 源行跨度(字节);驱动给的 Y stride,== src_w 时传 src_w
 * @return DG_OK / DG_ERR_PARAM / DG_ERR_IO(RGA 失败)
 */
int npu_pre_nv12_letterbox_rgb(const uint8_t *nv12, int stride,
                               const npu_letterbox_t *lb, uint8_t *dst_rgb);

/**
 * RGA:从 NV12 帧裁一块矩形(可同时缩放)→ RGB888。识别用:把检测到的人脸
 * 区域按**原始分辨率**抠出来再做 112×112 对齐——直接从 320×320 letterbox 画布
 * 上对齐等于把脸放大 4 倍再喂识别,精度白白损失。
 * @param rect 裁剪矩形(源图坐标;x/y/w/h 须为偶数,YUV420 色度对齐要求)
 * @return DG_OK / DG_ERR_PARAM / DG_ERR_IO
 */
int npu_pre_nv12_crop_rgb(const uint8_t *nv12, int stride,
                          int src_w, int src_h,
                          int rx, int ry, int rw, int rh,
                          uint8_t *dst_rgb, int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif

#endif /* DG_NPU_PRE_H */
