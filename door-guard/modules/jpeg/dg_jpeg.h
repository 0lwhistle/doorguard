/*
 * dg_jpeg.h — JPEG 编解码模块(libjpeg 薄封装,内存↔内存)
 *
 * 用途:录入头像的"拍摄时编码 / 显示时解码"。照片是 KB 级数据,
 * 按架构纪律不进事件总线,也不进任何结构体热路径——只在拍下的那一刻
 * 与显示刷新时各用一次。板上与 sysroot 均为 libjpeg-turbo(/usr/lib/libjpeg.so.8),
 * 宿主测试装 libjpeg-dev,同一份源码双端编译。
 *
 * 为什么放 modules/:自包含编解码能力,不含业务语义;视觉(编码)与
 * UI(解码)都只读直调本模块(登记制只读直调,见 architecture-v2 §1)。
 */
#ifndef DG_JPEG_H
#define DG_JPEG_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RGB888 → JPEG(内存输出)。
 * @param quality 1~95(头像用 80:160×160 约 6~10KB)
 * @param out_len 出参,写入字节数
 * @return DG_OK / DG_ERR_PARAM / DG_ERR_NO_MEMORY(容量不足,显式报错不截断)
 */
int dg_jpeg_encode_rgb(const uint8_t *rgb888, int w, int h, int quality,
                       uint8_t *out, size_t cap, size_t *out_len);

/**
 * JPEG → RGB888(内存输入)。损坏输入显式报错,不崩不静默。
 * @param scale_denom 缩小解码倍率 {1,2,4,8}:libjpeg 的 DCT 缩放解码,
 *        缩略图用它省内存省 CPU(160/4=40 正好是列表缩略图尺寸)
 * @param w,h   出参:实际输出尺寸(160/4=40 这类整除值)
 * @param cap   out 容量;不足返回 DG_ERR_NO_MEMORY
 */
int dg_jpeg_decode_rgb(const uint8_t *jpeg, size_t len, int scale_denom,
                       uint8_t *out_rgb888, size_t cap, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* DG_JPEG_H */
