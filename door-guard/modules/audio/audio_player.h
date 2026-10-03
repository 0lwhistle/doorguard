/*
 * audio_player.h — 语音播报播放器(modules/audio 对外总入口)
 *
 * 单线程播放 + 定长作业队列(FIFO):提示类音源宁排队不并发,否则两条
 * 提示叠在一起等于没提示。队列满丢新并计数(提示音迟到不如不响)。
 *
 * 音源两级降级:
 *   1. audio_prompt_play(name) → <cfg audio.prompt_dir>/<name>.wav 存在
 *      才播;缺席降级为内置提示音(开源箱即有声,素材后补);
 *   2. 后端打不开(硬件未接入)→ 静默跳过 + EV_AUDIO_STATE 降级事件,
 *      每 5s 懒重试(硬件接入后无需重启自动恢复)。
 *
 * 业务挂接(默认已接,详情见 README):开门 → success 提示,验证拒绝 →
 * fail 提示;audio_play_tone/audio_play_file 是通用口,后续功能直接用。
 */
#ifndef DG_AUDIO_PLAYER_H
#define DG_AUDIO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动(幂等):读 cfg、选后端、起播放线程、订阅开门/拒绝提示事件。
 *  audio_enabled=0 或无后端 → 不报错,静默空转(降级语义)。 */
int audio_player_start(void);

/** 停服(幂等):停线程、关后端、撤订阅 */
void audio_player_stop(void);

/** 后端当前可用(打开成功或尚未失败到降级) */
bool audio_player_ready(void);

/** 播内置提示音(任意线程,投队列即回);freq 0 = 静默占位 */
int audio_play_tone(double freq_hz, uint32_t dur_ms);

/** 播 WAV 文件(48k/16bit/单或双声道;其他规格拒绝,见 wav.h) */
int audio_play_file(const char *wav_path);

/** 播命名提示:<prompt_dir>/<name>.wav;文件缺席降级为内置提示音 */
int audio_prompt_play(const char *name);

/** 看门狗心跳(播放线程活性) */
int64_t audio_player_heartbeat_ms(void);

/** 仅测试用:等播放队列排空(最长 timeout_ms;宿主 sink 消费瞬时) */
void audio_player_test_wait_idle(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DG_AUDIO_PLAYER_H */
