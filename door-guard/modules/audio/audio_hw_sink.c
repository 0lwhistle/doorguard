/*
 * audio_hw_sink.c — 宿主测试后端:PCM 落内存(可断言),零硬件依赖
 *
 * 只为 tests/ 服务:录最近 N 帧环形 + 帧计数 + 峰值,断言播放内容/时长/
 * 音量不依赖真扬声器。板上构建不编译(宿主与板共用同一 CMake 源列表的
 * 例外:本文件恒编——ALSA 板上也无害,只是没人链接它)。
 */
#include "audio_hw.h"

#include <pthread.h>
#include <string.h>

#define SINK_MAX_FRAMES (48 * 1000 * 2)     /* 2 秒 @48k 帧(够覆盖提示音) */

static int16_t s_buf[SINK_MAX_FRAMES * 2];
static size_t s_total;                      /* 累计帧数(不回绕) */
static size_t s_wr;                         /* 环形写位(帧) */
static unsigned s_rate, s_ch;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

void audio_sink_reset(void)
{
    pthread_mutex_lock(&s_mtx);
    s_total = 0;
    s_wr = 0;
    memset(s_buf, 0, sizeof(s_buf));
    pthread_mutex_unlock(&s_mtx);
}

size_t audio_sink_frames(void)
{
    pthread_mutex_lock(&s_mtx);
    size_t v = s_total;
    pthread_mutex_unlock(&s_mtx);
    return v;
}

/** 取第 idx 帧(绝对帧号)左右声道;越界返回 false */
bool audio_sink_frame(size_t idx, int16_t *l, int16_t *r)
{
    pthread_mutex_lock(&s_mtx);
    bool ok = idx < s_total && idx < SINK_MAX_FRAMES;
    if (ok) {
        *l = s_buf[(idx % SINK_MAX_FRAMES) * 2];
        *r = s_buf[(idx % SINK_MAX_FRAMES) * 2 + 1];
    }
    pthread_mutex_unlock(&s_mtx);
    return ok;
}

/** 全体样本绝对值峰值(音量/静音断言用) */
int audio_sink_peak(void)
{
    pthread_mutex_lock(&s_mtx);
    int peak = 0;
    size_t n = s_total < SINK_MAX_FRAMES ? s_total : SINK_MAX_FRAMES;
    for (size_t i = 0; i < n * 2; i++) {
        int a = s_buf[i] < 0 ? -s_buf[i] : s_buf[i];
        if (a > peak)
            peak = a;
    }
    pthread_mutex_unlock(&s_mtx);
    return peak;
}

static int sink_open(const char *device, unsigned rate, unsigned channels)
{
    (void)device;
    pthread_mutex_lock(&s_mtx);
    s_rate = rate;
    s_ch = channels;
    s_total = 0;
    s_wr = 0;
    memset(s_buf, 0, sizeof(s_buf));
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

static int sink_write(const int16_t *interleaved, size_t n_frames)
{
    if (!interleaved)
        return DG_ERR_PARAM;
    pthread_mutex_lock(&s_mtx);
    for (size_t f = 0; f < n_frames && s_total < SINK_MAX_FRAMES; f++) {
        s_buf[s_wr * 2] = interleaved[f * 2];
        s_buf[s_wr * 2 + 1] = interleaved[f * 2 + 1];
        s_wr = (s_wr + 1) % SINK_MAX_FRAMES;
        s_total++;
    }
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

static void sink_close(void)
{
}

const audio_hw_ops_t audio_sink_ops = {
    .name = "sink",
    .open = sink_open,
    .write = sink_write,
    .close = sink_close,
};
