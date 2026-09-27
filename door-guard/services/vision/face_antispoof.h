/*
 * face_antispoof.h — 反欺骗(MiniFASNet)的纯算法件:几何/通道/时序平滑
 *
 * 分层(与 rknn_face.c 同理由:NPU 无关的数学留在宿主可测处):
 *   本文件      crop 几何(官方 _get_new_box 复刻)/ RGB→BGR / 多帧中位数平滑
 *   装配        vision_rknn.c:RGA crop → 双模型推理(softmax 已烤进 rknn)→
 *               real 概率(输出[1])→ 本文件平滑 → 判定 → EV_VISION_MATCH_1N 的
 *               spoof_challenge 标志
 *   业务        access 的 auth_fsm:命中 + spoof_challenge → 多模态二次验证
 *
 * 模型规格(权威=third_party/Silent-Face-Anti-Spoofing-master,转换见
 * tools/convert_antispoof/):
 *   - 两个 80×80 模型各自按「检测框 ×scale」取景:V2=2.7、V1SE=4.0,
 *     概率相加取 argmax,label==1 才是真脸(其余一律按假体计);
 *   - 输入 BGR **原域 [0,255]**——官方 to_tensor 不做 /255(易踩!),
 *     板上喂 uint8 BGR,rknn config 不设归一化;
 *   - 输入矩形须偶对齐(YUV420 色度要求),本文件的几何函数负责收缩。
 */
#ifndef DG_FACE_ANTISPOOF_H
#define DG_FACE_ANTISPOOF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANTISPOOF_PATCH_SZ 80   /**< 模型输入边长 */
#define ANTISPOOF_SCALE_A  2.7f /**< MiniFASNetV2 的取景倍率 */
#define ANTISPOOF_SCALE_B  4.0f /**< MiniFASNetV1SE 的取景倍率 */
#define ANTISPOOF_SMOOTH_N 5    /**< 中位数平滑窗口(帧;≈1.5s @300ms 节流) */

/** 官方 CropImage._get_new_box:检测框按 scale 扩大后中心不变,整体平移
 *  夹回图内。输入检测框 (x,y,w,h) 与图幅,输出 (x,y,w,h) 模型取景矩形。
 *  w/h 收缩为偶数(只缩不涨,仍含框心);图幅装不下时返回可见部分。 */
void antispoof_scale_box(int box_x, int box_y, int box_w, int box_h,
                         int src_w, int src_h, float scale,
                         int *out_x, int *out_y, int *out_w, int *out_h);

/** RGB888 → BGR888(模型按 cv2.imread 的 BGR 训练;板上 RGA 产 RGB)。 */
void antispoof_rgb_to_bgr(const uint8_t *rgb, uint32_t n_pixels, uint8_t *bgr);

/** 时序平滑器:滑动窗口取中位数。假脸分数帧间抖动大、真脸稳定,
 *  中位数既压单帧毛刺又不跟着离群帧走。窗口不满时按已有帧计。
 *  reset 后首次 push 立即有输出。 */
typedef struct {
    float win[ANTISPOOF_SMOOTH_N];
    int   count;                /**< 已入队的帧数(≤ ANTISPOOF_SMOOTH_N) */
    int   head;                 /**< 下一写入位 */
} antispoof_smooth_t;

void  antispoof_smooth_reset(antispoof_smooth_t *s);
/** push 一帧 real 概率,返回当前窗口中位数 */
float antispoof_smooth_push(antispoof_smooth_t *s, float real_prob);

/** 判定:平滑后的 real 概率低于阈值 → 疑似假体(命中不放行,发起二次验证)。
 *  降级方案下误拒只多验一道,阈值可调严(threshold 走 cfg)。 */
bool antispoof_is_spoof(float smooth_real, float threshold);

#ifdef __cplusplus
}
#endif

#endif /* DG_FACE_ANTISPOOF_H */
