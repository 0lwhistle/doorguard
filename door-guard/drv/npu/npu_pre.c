/*
 * npu_pre.c — RGA 硬件预处理(NV12 → letterbox RGB888)
 *
 * 为什么用 RGA 而不是软件缩放:1280×720 NV12 缩到 320×320 并转 RGB 是每帧
 * 数十万像素的活,软做会吃掉主循环预算;RGA 单帧亚毫秒(与 camera 模块显示通路
 * 同一套硬件,已验证)。
 *
 * 步骤(一次 improcess + 一次 imfill):
 *   1. imfill 整画布补 114(letterbox 的边);
 *   2. improcess 把 NV12 等比缩放进居中区域,同时完成 YUV→RGB 与尺寸变换。
 *
 * 坐标系源:librga 的 RK_FORMAT_RGB_888 是 R,G,B 字节序(与模型要的 RGB 一致);
 * 若板上实测"完全检不出脸"而分数异常,先怀疑这里应为 BGR(改一个常量即可验证)。
 */
#include "npu_pre.h"
#include "dg_log.h"

#include <rga/im2d.h>
#include <rga/RgaApi.h>

#include <string.h>

static const char *TAG = "[NPU]";

/* librga 有两个成功码:SUCCESS=1 / NOERROR=2,只认其一会把成功当失败
 * (与 camera_board.c 的 RGA 用法同一教训) */
static int rga_ok(IM_STATUS st)
{
    return st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR;
}

int npu_pre_nv12_letterbox_rgb(const uint8_t *nv12, int stride,
                               const npu_letterbox_t *lb, uint8_t *dst_rgb)
{
    if (!nv12 || !lb || !dst_rgb)
        return DG_ERR_PARAM;
    if (lb->src_w <= 0 || lb->src_h <= 0 || lb->dst_w <= 0 || lb->dst_h <= 0)
        return DG_ERR_PARAM;
    if (stride < lb->src_w)
        stride = lb->src_w;             /* 驱动未给 stride 时按紧凑排布 */

    /* 源:整个 NV12 帧 */
    rga_buffer_t src = wrapbuffer_virtualaddr_t((void *)nv12,
                                                lb->src_w, lb->src_h,
                                                stride, lb->src_h,
                                                RK_FORMAT_YCbCr_420_SP);
    /* 目标:模型输入画布(RGB888,紧凑排布) */
    rga_buffer_t dst = wrapbuffer_virtualaddr_t(dst_rgb,
                                                lb->dst_w, lb->dst_h,
                                                lb->dst_w, lb->dst_h,
                                                RK_FORMAT_RGB_888);

    /* 1. 补边(整画布) */
    im_rect full = { 0, 0, lb->dst_w, lb->dst_h };
    const int pad = NPU_PRE_PAD_VALUE;
    const int pad_color = (pad << 16) | (pad << 8) | pad;   /* 0x00727272 */
    IM_STATUS st = imfill_t(dst, full, pad_color, 1);
    if (!rga_ok(st)) {
        DG_LOGE(TAG, "RGA 补边失败(%s)", imStrError_t(st));
        return DG_ERR_IO;
    }

    /* 2. 等比缩放 + YUV→RGB,画进居中区域 */
    im_rect srect = { 0, 0, lb->src_w, lb->src_h };
    im_rect drect = { lb->pad_x, lb->pad_y, lb->fit_w, lb->fit_h };
    im_rect prect = { 0, 0, 0, 0 };
    st = improcess(src, dst, (rga_buffer_t){ 0 }, srect, drect, prect, IM_SYNC);
    if (!rga_ok(st)) {
        DG_LOGE(TAG, "RGA letterbox 失败(%s)", imStrError_t(st));
        return DG_ERR_IO;
    }
    return DG_OK;
}

int npu_pre_nv12_rotate(const uint8_t *nv12, int stride, int w, int h,
                        uint8_t *dst, int rot_deg)
{
    if (!nv12 || !dst)
        return DG_ERR_PARAM;
    if (w <= 0 || h <= 0)
        return DG_ERR_PARAM;
    if (rot_deg != 0 && rot_deg != 90 && rot_deg != 180 && rot_deg != 270)
        return DG_ERR_PARAM;
    if (stride < w)
        stride = w;
    if (rot_deg == 0) {                     /* 同向:复制一份,调用方域逻辑不变 */
        memcpy(dst, nv12, (size_t)stride * h * 3 / 2);
        return DG_OK;
    }

    rga_buffer_t src = wrapbuffer_virtualaddr_t((void *)nv12, w, h, stride, h,
                                                RK_FORMAT_YCbCr_420_SP);
    const int dw = (rot_deg == 90 || rot_deg == 270) ? h : w;
    const int dh = (rot_deg == 90 || rot_deg == 270) ? w : h;
    rga_buffer_t dout = wrapbuffer_virtualaddr_t(dst, dw, dh, dw, dh,
                                                 RK_FORMAT_YCbCr_420_SP);
    const IM_STATUS st = imrotate_t(src, dout,
                                    rot_deg == 90  ? IM_HAL_TRANSFORM_ROT_90
                                    : rot_deg == 180 ? IM_HAL_TRANSFORM_ROT_180
                                                     : IM_HAL_TRANSFORM_ROT_270,
                                    IM_SYNC);
    if (!rga_ok(st)) {
        DG_LOGE(TAG, "RGA NV12 旋转失败(%s)", imStrError_t(st));
        return DG_ERR_IO;
    }
    return DG_OK;
}

int npu_pre_nv12_crop_rgb(const uint8_t *nv12, int stride,
                          int src_w, int src_h,
                          int rx, int ry, int rw, int rh,
                          uint8_t *dst_rgb, int dst_w, int dst_h)
{
    if (!nv12 || !dst_rgb)
        return DG_ERR_PARAM;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return DG_ERR_PARAM;
    /* 越界/奇数坐标直接拒:YUV420 裁剪奇数偏移会拿到错色度,静默出错图 */
    if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0 ||
        rx + rw > src_w || ry + rh > src_h ||
        (rx & 1) || (ry & 1) || (rw & 1) || (rh & 1))
        return DG_ERR_PARAM;
    if (stride < src_w)
        stride = src_w;

    rga_buffer_t src = wrapbuffer_virtualaddr_t((void *)nv12,
                                                src_w, src_h, stride, src_h,
                                                RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr_t(dst_rgb,
                                                dst_w, dst_h, dst_w, dst_h,
                                                RK_FORMAT_RGB_888);
    im_rect srect = { rx, ry, rw, rh };
    im_rect drect = { 0, 0, dst_w, dst_h };
    im_rect prect = { 0, 0, 0, 0 };
    IM_STATUS st = improcess(src, dst, (rga_buffer_t){ 0 }, srect, drect, prect,
                             IM_SYNC);
    if (!rga_ok(st)) {
        DG_LOGE(TAG, "RGA 裁剪失败(%s)", imStrError_t(st));
        return DG_ERR_IO;
    }
    return DG_OK;
}
