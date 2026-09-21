/*
 * face_quality.h — 人脸质量闸门(纯 C,宿主可测)
 *
 * 为什么需要:检测到脸 ≠ 能拿这张脸做识别/入库。抖动造成的运动模糊、脸太小、
 * 侧脸角度过大,都会让特征可信度骤降——喂进去的坏特征既可能误判(错开门),
 * 也可能污染库(录进一张糊脸,以后本人都刷不开)。用户明确提过这条需求。
 *
 * 三个因子(够用且都可解释,不做花哨评分模型):
 *   ① 清晰度 = 灰度 Laplacian 方差(越小越糊;越大越清晰)——抖动模糊的克星
 *   ② 人脸尺寸 = 检测框较小边像素(太小则像素不足以支撑 512 维特征)
 *   ③ 检测置信度 = 检测器输出的分数
 * 姿态(yaw)暂不做:5 点估角误差大,且①②已覆盖"抖动糊脸"这个主要诉求;
 * 将来接 68/106 点后可加,接口留了 pose 位。
 *
 * 阈值一律走配置(零魔数纪律),调用方用 face_quality_cfg_from() 取。
 * 判定结果带"哪一项不合格",便于 UI 显示具体提示("太模糊,请保持不动"),
 * 而不是笼统的"验证失败"。
 */
#ifndef DG_FACE_QUALITY_H
#define DG_FACE_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 质量判定不合格的原因(0 = 合格;UI 据此给具体提示) */
typedef enum {
    FQ_OK = 0,
    FQ_ERR_SMALL,      /**< 人脸太小(离远/检测框过小) */
    FQ_ERR_BLURRY,     /**< 太模糊(抖动/失焦) */
    FQ_ERR_LOW_SCORE,  /**< 检测置信度不足 */
    FQ_ERR_PARAM,      /**< 入参非法 */
} face_quality_verdict_t;

/** 质量阈值(来自 cfg;全部 >0 才生效,=0 表示该项不启用) */
typedef struct {
    int32_t min_face_px;    /**< 人脸框较小边最小像素 */
    double  blur_min;       /**< Laplacian 方差下限 */
    double  det_score_min;  /**< 检测分数下限(0~1) */
} face_quality_thr_t;

/** 一次质量测量结果(可日志可调试,便于板上标定阈值) */
typedef struct {
    int32_t face_px;        /**< 人脸框较小边(像素) */
    double  det_score;      /**< 检测分数 */
    double  blur;           /**< Laplacian 方差 */
} face_quality_t;

/**
 * 灰度 Laplacian 方差(清晰度指标)。
 * 4 邻域 Laplacian,边界像素跳过;返回方差;区域过小返回 0。
 * 纯算术,宿主可直接用合成图验证(见 tests/test_face_quality.c)。
 */
double face_quality_blur(const uint8_t *gray, int w, int h);

/** RGB888 → 灰度(整数近似 0.299/0.587/0.114),out 与 rgb 同尺寸 */
void face_quality_gray(const uint8_t *rgb, int w, int h, uint8_t *gray);

/**
 * 判定:逐项比对阈值,返回第一个不合格项(FQ_OK = 全过)。
 * thr 中某项 <=0 视为不启用该项(便于板上先只开清晰度)。
 */
face_quality_verdict_t face_quality_check(const face_quality_t *q,
                                          const face_quality_thr_t *thr);

/** 原因 → 稳定英文标识(日志用;UI 文案另经 _() 映射,不耦合) */
const char *face_quality_reason_str(face_quality_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* DG_FACE_QUALITY_H */
