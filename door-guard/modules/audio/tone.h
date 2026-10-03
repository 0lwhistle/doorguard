/*
 * tone.h — 提示音正弦合成(modules/audio;纯函数,宿主可测)
 *
 * 跨 chunk 连续:相位由调用方携带(同一提示音分多次 write 时不断波)。
 * 音量/淡入淡出由播放器做(需要总时长,这里保持纯渲染)。
 */
#ifndef DG_TONE_H
#define DG_TONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 渲染 n_frames 帧(立体声交错)到 dst。
 *  @param freq_hz 频率(Hz);0 = 静音(仍填 0,保节奏)
 *  @param vol     0.0~1.0 线性幅度
 *  @param phase   相位累加器(弧度),调用方持有、跨 chunk 传递;NULL = 内部弃置 */
void tone_render(int16_t *dst, size_t n_frames, unsigned rate, unsigned channels,
                 double freq_hz, double vol, double *phase);

#ifdef __cplusplus
}
#endif

#endif /* DG_TONE_H */
