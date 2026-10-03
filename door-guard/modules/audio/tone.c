/*
 * tone.c — 正弦提示音渲染实现
 *
 * 幅度上限取 0.95×32767(留头部余量,功放端再吃削顶就难听了);相位步进
 * 用 double(48k 下 float32 在低频段累计误差听得出来,双精度无感)。
 */
#include "tone.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TONE_AMP_MAX (32767.0 * 0.95)

void tone_render(int16_t *dst, size_t n_frames, unsigned rate, unsigned channels,
                 double freq_hz, double vol, double *phase)
{
    if (!dst || channels == 0)
        return;
    double local_phase = 0.0;
    if (!phase)
        phase = &local_phase;

    const double step = 2.0 * M_PI * (freq_hz > 0.0 ? freq_hz : 0.0) / rate;
    const double amp = TONE_AMP_MAX * (vol < 0.0 ? 0.0 : (vol > 1.0 ? 1.0 : vol));
    for (size_t f = 0; f < n_frames; f++) {
        const int16_t s = freq_hz > 0.0
                              ? (int16_t)lround(amp * sin(*phase))
                              : 0;
        for (unsigned c = 0; c < channels; c++)
            dst[f * channels + c] = s;
        *phase += step;
        if (*phase >= 2.0 * M_PI)
            *phase -= 2.0 * M_PI;
    }
}
