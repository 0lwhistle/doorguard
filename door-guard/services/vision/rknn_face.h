/*
 * rknn_face.h — 自组 rknn 人脸后处理(解码 / NMS / 5 点对齐 / 特征比对)
 *
 * 纯 C、零硬件依赖:全是数学与坐标变换,在 WSL 宿主上就能单测
 * (tests/test_rknn_face.c)。硬件侧(喂帧/推理)在 drv/npu,
 * 契约装配与事件发布在 vision_rknn.c —— 三层各管一段。
 *
 * 为什么解码单独成单元:解码规则随检测器走,换检测器只换解码。
 * 当前实现 **SCRFD-10G**(`det_10g.rknn`,板上实测见 models/README.md):
 *   输入 640×640×3 NHWC F16;9 个输出 = 3 个 stride × (score/bbox/kps),
 *   每位置 2 anchor;stride 8/16/32 → 12800/3200/800。
 *   输出顺序 = [s8,s16,s32, b8,b16,b32, k8,k16,k32](rknn_model_zoo 约定)。
 *   bbox/kps 是"相对 anchor 中心的距离 × stride",需按 stride 还原。
 *
 * RetinaFace 若要接(需先按 RK3576 重转,见 models/README):它是 anchor-based
 * 方案,解码另写一个函数,本文件的 NMS/对齐/比对可原样复用。
 *
 * 坐标空间约定:本文件产出的坐标一律在**模型输入(letterbox)空间**;
 * 逆映射回相机/屏幕坐标由调用方用 letterbox 参数做(不在本文件,避免耦合)。
 */
#ifndef DG_RKNN_FACE_H
#define DG_RKNN_FACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 关键点数(InsightFace 5 点:左眼/右眼/鼻/左嘴角/右嘴角) */
#define RKNN_FACE_KPS 5

/** 一个人脸(坐标在模型输入/letterbox 空间) */
typedef struct {
    float x1, y1, x2, y2;          /**< 检测框 */
    float score;                   /**< 置信度(0~1) */
    float kps[RKNN_FACE_KPS][2];   /**< 5 关键点 */
} rknn_face_t;

/** SCRFD 一档 stride 的三个输出张量(指向 NPU 取出的 float 缓冲,不拥有) */
typedef struct {
    const float *score;   /**< [grid_w*grid_h*n_anchors] */
    const float *bbox;    /**< 同上 × 4(左上右下距离,单位 stride) */
    const float *kps;     /**< 同上 × 10(5 点 x,y,单位 stride) */
    int grid_w;           /**< 网格宽(输入边长 / stride) */
    int grid_h;           /**< 网格高 */
    int n_anchors;        /**< 每位置 anchor 数(SCRFD-10G = 2) */
    int stride;           /**< 该档 stride(8/16/32) */
} rknn_scrfd_level_t;

/**
 * 解码 SCRFD 输出为候选框(不做 NMS,不按面积排序)。
 * @param lv           各档张量(通常 3 档)
 * @param n_lv         档数
 * @param score_thresh 入框阈值(建议 0.5,实测再调)
 * @param out          输出数组
 * @param max_out      out 容量
 * @return **命中的候选框总数**(可能 > max_out,此时 out 只写了 max_out 个
 *         —— 调用方据此知道被截断多少,不是静默丢弃);<0 = 参数非法
 */
int rknn_scrfd_decode(const rknn_scrfd_level_t *lv, int n_lv, float score_thresh,
                      rknn_face_t *out, int max_out);

/**
 * RetinaFace 解码(与 rknn_model_zoo `examples/RetinaFace/python/RetinaFace.py`
 * 的 PriorBox / box_decode / decode_landm 逐条对齐)。
 * 锚框方案:min_sizes=[[16,32],[64,128],[256,512]]、steps=[8,16,32],
 * 归一化 [cx,cy,w,h] 形式(中心 +0.5 格偏移);方差 [0.1, 0.2];
 * 320 输入 → 40²×2 + 20²×2 + 10²×2 = **4200** 个锚框。
 * 输出坐标在模型输入(letterbox)像素空间,×size 已还原。
 *
 * @param loc    [n*4]  框回归输出
 * @param conf   [n*2]  分类输出(softmax 已含);本函数取 [.,1] 作人脸分
 * @param landm  [n*10] 关键点回归输出
 * @param n      锚框数
 * @param size   模型输入边长(320)
 * @return 命中总数(可能 > max_out);<0 = 参数非法;
 *         **-2 = 锚框数与 size 不匹配**(解码方案与模型不符,响亮报错而非给出错框)
 */
int rknn_retinaface_decode(const float *loc, const float *conf, const float *landm,
                           int n, int size, float score_thresh,
                           rknn_face_t *out, int max_out);

/** 按 size 推算该有的锚框数(供调用方分配缓冲/自检) */
int rknn_retinaface_anchor_count(int size);

/** IoU(交并比);退化框返回 0 */
float rknn_iou(const rknn_face_t *a, const rknn_face_t *b);

/**
 * 贪心 NMS:**原地**按 score 降序保留、抑制重叠(>iou_thresh)。
 * @return 保留数(前 n_return 个元素有效)
 */
int rknn_nms(rknn_face_t *faces, int n, float iou_thresh);

/**
 * 5 点对齐:求 2×3 仿射矩阵,把 src_kps 映射到 ArcFace 的 112×112 参考布局
 * (InsightFace arcface_src 五点,最小二乘相似变换——只有缩放/旋转/平移 4 自由度,
 *  不用透视变换:人脸是刚体近似,多出来的自由度只会拟合噪声)。
 * @param m 输出 [m0 m1 m2; m3 m4 m5]:X = m0*x + m1*y + m2,Y = m3*x + m4*y + m5
 * @return DG 风格错误码(0 成功)
 */
int rknn_align_plan(const float src_kps[RKNN_FACE_KPS][2], float m[6]);

/**
 * 按 m 把 src 的 RGB888 重采样到 dst(双线性,逆映射采样;越界填 0)。
 * 常规用法:rknn_align_warp(..., dst, 112, 112) 得到 ArcFace 输入。
 */
void rknn_align_warp(const uint8_t *src, int sw, int sh, const float m[6],
                     uint8_t *dst, int dw, int dh);

/** 原地 L2 归一化(零向量保持不变) */
void rknn_l2_normalize(float *v, int n);

/** 余弦相似度(0~1 视用途;未归一化也正确,已归一化则退化为点积) */
float rknn_cosine(const float *a, const float *b, int n);

/**
 * RGB888 → ArcFace 输入:逐像素 (x-127.5)/127.5 写入 float32。
 * 板上实测(2026-09-21,models/README ⑤)w600k_r50 **未烤入归一化**,
 * 必须喂预归一化 float——直接喂 uint8 会让全部 embedding 高度相似
 * (cos(脸,纯色)≈0.79,识别"永远不命中"且无任何报错)。
 */
void rknn_rgb_norm_f32(const uint8_t *rgb, int n_pixels, float *out);

#ifdef __cplusplus
}
#endif

#endif /* DG_RKNN_FACE_H */
