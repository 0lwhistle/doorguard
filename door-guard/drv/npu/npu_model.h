/*
 * npu_model.h — NPU 推理库(RKNN 运行时薄封装,drv 层机制库)
 *
 * 定位:只回答"怎么把一个 .rknn 跑起来"——加载 / 查张量 / 喂输入 / 推理 /
 * 取输出 / 释放。**不认识人脸**,不认识 RetinaFace/SCRFD/ArcFace:任何 rknn
 * 模型(检测/识别/关键点/将来的活体)共用本库。
 * 模型专属的后处理(解码/NMS/关键点对齐/特征比对)不放这里而留在
 * services/vision——驱动层一旦认识具体模型,换个模型就得改驱动,那是分层塌陷。
 *
 * 输入尺寸不写死:转换时的输入尺寸/布局/类型随模型文件走,加载后用
 * npu_model_input_attr() 查出来,调用方按返回值准备缓冲。换模型(含换输入
 * 分辨率)不必改代码,也不需要知道当初 rknn-toolkit2 是怎么配的。
 *
 * 线程安全:单个 npu_model_t 句柄只在一个线程里用(rknn context 非线程安全);
 * 需要并发就各自 load 一个句柄。这条是硬约束,不要靠加锁绕过。
 *
 * 内存:句柄持有 rknn context 与"最近一次"输出;输出在下一次 run 或 release
 * 时释放,调用方必须在下次 run 前把要用的输出拷走。
 */
#ifndef DG_NPU_MODEL_H
#define DG_NPU_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 张量数据类型(本库自己的枚举:不把 rknn_api.h 泄漏给上层) */
typedef enum {
    DG_NPU_TYPE_U8 = 0,
    DG_NPU_TYPE_I8,
    DG_NPU_TYPE_F16,
    DG_NPU_TYPE_F32,
    DG_NPU_TYPE_I32,
    DG_NPU_TYPE_OTHER,
} dg_npu_type_t;

/* 张量布局 */
typedef enum {
    DG_NPU_FMT_NCHW = 0,
    DG_NPU_FMT_NHWC,
    DG_NPU_FMT_OTHER,
} dg_npu_fmt_t;

/* 张量属性(裁剪自 rknn_tensor_attr:只曝推理侧真正要用的字段) */
typedef struct {
    int      width;      /**< 宽(按 NHWC 语义解出的 W) */
    int      height;     /**< 高 */
    int      channels;   /**< 通道数 */
    uint32_t dims[4];    /**< 原始维度(个数见 n_dims) */
    uint32_t n_dims;
    dg_npu_type_t type;  /**< 数据类型 */
    dg_npu_fmt_t  fmt;   /**< 布局 */
    bool     quant;      /**< 是否量化;true 时 zp/scale 有效 */
    int32_t  zp;         /**< 量化零点(affine asymmetric) */
    float    scale;      /**< 量化尺度:实数 = (量化值 - zp) * scale */
    uint32_t elems;      /**< 元素总数 */
    size_t   nbytes;     /**< 该张量按 type 的字节数 */
} npu_attr_t;

/** 不透明句柄(一个模型一个) */
typedef struct npu_model npu_model_t;

/** 运行时与驱动版本(启动日志/排障用;"api=… drv=…") */
const char *npu_hal_version(void);

/**
 * 加载 .rknn 模型。
 * @param path 模型文件路径(.rknn)
 * @return 句柄;NULL = 失败(原因已进日志:文件缺失/格式不符/驱动不匹配)
 */
npu_model_t *npu_model_load(const char *path);

/** 释放句柄(含 rknn context 与未取的输出);传 NULL 安全 */
void npu_model_release(npu_model_t *m);

/** 输入/输出张量个数(查询失败返回 0) */
uint32_t npu_model_input_num(const npu_model_t *m);
uint32_t npu_model_output_num(const npu_model_t *m);

/** 查询第 idx 个输入/输出张量的属性(越界/查询失败返回 DG_ERR_PARAM / DG_ERR_IO) */
int npu_model_input_attr(const npu_model_t *m, uint32_t idx, npu_attr_t *out);
int npu_model_output_attr(const npu_model_t *m, uint32_t idx, npu_attr_t *out);

/**
 * 跑一帧:设输入 → 推理 → 取输出(want_float,驱动负责反量化)。
 *
 * @param in_bytes 输入缓冲字节数(与输入张量尺寸不符返回 DG_ERR_PARAM,不静默)
 * @param in_type  **缓冲里实际是什么**(不是模型要什么):最常用
 *        `DG_NPU_TYPE_U8` —— 原始 uint8 图像,由运行时按模型量化参数转换
 *        (RetinaFace 这类把 mean/std 烤进图的模型就该这样喂);
 *        `DG_NPU_TYPE_F16` —— 已按模型要求归一化好的半精度数据(SCRFD 是这种);
 *        传 `DG_NPU_TYPE_OTHER` = 按模型自带类型原样送,不做转换。
 *        这个参数不能省:int8 与 uint8 的缓冲**字节数相同**,喂错类型不会报错,
 *        只会静默出错图——必须显式声明。
 * @return DG_OK / DG_ERR_NOT_INIT / DG_ERR_PARAM / DG_ERR_IO
 */
int npu_model_run(npu_model_t *m, const void *input, size_t in_bytes,
                  dg_npu_type_t in_type);

/**
 * 取第 idx 个输出为 float32(驱动已反量化)。下次 run 后失效。
 * @param cap_elems dst 容量(元素数);不够返回 DG_ERR_PARAM,不截断
 * @param n_out     实际元素数(可 NULL)
 */
int npu_model_output_f32(const npu_model_t *m, uint32_t idx,
                         float *dst, uint32_t cap_elems, uint32_t *n_out);

/** 加载时用的模型路径(日志用) */
const char *npu_model_path(const npu_model_t *m);

#ifdef __cplusplus
}
#endif

#endif /* DG_NPU_MODEL_H */
