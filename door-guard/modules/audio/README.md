# audio — 语音播报(MAX98357A I2S 功放 + 扬声器)

> 2026-10-04 落地(任务一)。硬件尚未接线,代码与功能已就位——**接入后
> 零代码改动**:dts 使能声卡,配置指对 PCM 名即出声。

## 分层

```
audio_player.c   播放器:作业队列(FIFO 4)+ 单播放线程 + 两级降级
  ├─ wav.c       WAV(RIFF/PCM16)头解析,纯函数
  ├─ tone.c      提示音正弦合成(相位跨 chunk 连续),纯函数
  └─ audio_hw.c  后端选择/注入
       ├─ audio_hw_alsa.c  板上:ALSA PCM(CMake DG_AUDIO_ALSA=1)
       └─ audio_hw_sink.c  宿主测试:PCM 落内存可断言
```

## 硬件接入说明(MAX98357A)

- 芯片是 **I2S 从片 D 类功放**:BCLK/LRCLK/DIN 接 RK3576 I2S TX,GND 共地,
  VIN 5V;SD 模式脚接法决定增益(悬空 9dB / 接 GND 12dB / 接 VIN 6dB /
  电阻可调),板上实测嫌响/嫌轻先动增益脚,细调再用 `audio.volume`。
- 软件**只当它是一条 ALSA PCM 输出**:dts 加 simple-audio-card(或厂商
  声卡)后 `/proc/asound/` 出卡,`audio.device` 保持 `default` 或钉 `hw:0,0`。
- 固定输出格式 **48000Hz / S16LE / 双声道**(提示音规格同此,见下)。

## 配置(default.json `audio` 段;json-only)

| 键 | 默认 | 说明 |
|---|---|---|
| `enabled` | `true` | 播报开关 |
| `device` | `default` | ALSA PCM 名 |
| `volume` | `80` | 软件音量 0~100(先淡入出防咔声,再乘音量) |
| `prompt_dir` | `/userdata/doorguard/audio` | 语音素材目录 |

## 素材规格(钉死,超规格拒播并有 WARN 日志)

- 格式:WAV PCM16,**48000Hz**,单声道或双声道,≤2MB/文件。
- 命名:`success.wav`(开门)、`fail.wav`(验证拒绝);**文件缺席自动降级为
  内置提示音**(开箱即有声,素材后补)。
- 产出:TTS 导出后用 ffmpeg 转规格:
  `ffmpeg -i in.mp3 -ar 48000 -sample_fmt s16 out.wav`

## API

```c
#include "audio/audio_player.h"

audio_play_tone(1318.5, 160);        /* 内置提示音(频率 Hz,时长 ms) */
audio_play_file("/path/x.wav");      /* 一次性文件播放 */
audio_prompt_play("success");        /* 命名提示:<prompt_dir>/success.wav */

/* 业务挂接已内置(总线订阅,见 audio_player.c):
 *   EV_AUTH_DOOR_OPEN      → success.wav(缺席→880+1319 双音)
 *   EV_AUTH_RESULT(reject) → fail.wav(缺席→330 低音)
 * 后续功能直接调上面三个 API,或照此订阅别的业务事件。 */
```

## 降级语义(硬件未接入 = 现状)

后端 `open` 失败 → `EV_AUDIO_STATE{ready=false}` + WARN 一次,作业静默跳过,
**每 5s 懒重试**——硬件接好当次重试即恢复,无需重启应用;业务层零感知。

## 测试

`tests/test_audio.c`(sink 后端注入,宿主直跑):

- WAV 解析判定表(格式/位深/声道/截断负例)
- 正弦:静音、分段渲染与整段一致(相位连续)、幅度域
- 播放器:落帧数=时长×48k、淡入首帧近零、音量幅度域、单声道左右同值、
  44.1k 拒播、命名提示命中/缺席、开门与拒绝事件挂接、PASS 不触发、幂等停服

```bash
./build-tests/test_audio
```
