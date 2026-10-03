/*
 * audio_hw.h — 音频输出后端接口(modules/audio)
 *
 * 为什么有这层:MAX98357 是 I2S 从片功放,软件侧就是「一条 ALSA PCM 输出
 * (48k/16bit/立体声,阻塞写)」;把"往哪写"抽象成 ops 后——板上挂 ALSA
 * 后端(sysroot 自带 libasound),宿主测试挂录单 sink 后端,播放器逻辑
 * 同一份源码。硬件接入前 ALSA open 失败 → 播放器降级静默(自动重试探回)。
 *
 * 线程契约:ops 由播放器单线程调用(open 后 write…close),后端免锁。
 */
#ifndef DG_AUDIO_HW_H
#define DG_AUDIO_HW_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 固定输出格式:S16LE 交错;rate/channels 由 open 协商(本工程恒 48000/2) */
typedef struct {
    const char *name;                                  /**< 后端名(日志用) */
    int  (*open)(const char *device, unsigned rate_hz, unsigned channels);
    int  (*write)(const int16_t *interleaved, size_t n_frames); /**< DG_OK/IO */
    void (*close)(void);
} audio_hw_ops_t;

/** 板级默认后端:板上=ALSA;宿主测试构建=NULL(由测试注入 sink) */
const audio_hw_ops_t *audio_hw_board(void);

/** 注入后端(仅测试用;先于 audio_player_start 调用) */
void audio_hw_attach(const audio_hw_ops_t *ops);

/** 当前生效后端:注入优先,回落板级默认;无后端 = NULL(播放器降级) */
const audio_hw_ops_t *audio_hw_current(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_AUDIO_HW_H */
