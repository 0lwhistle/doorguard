/*
 * audio_hw_sink.h — 测试后端观察口(实现见 audio_hw_sink.c;仅 tests 使用)
 */
#ifndef DG_AUDIO_HW_SINK_H
#define DG_AUDIO_HW_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

void audio_sink_reset(void);
size_t audio_sink_frames(void);
bool audio_sink_frame(size_t idx, int16_t *l, int16_t *r);
int audio_sink_peak(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_AUDIO_HW_SINK_H */
