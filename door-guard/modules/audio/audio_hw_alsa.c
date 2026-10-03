/*
 * audio_hw_alsa.c — ALSA PCM 输出后端(板上;CMake DG_AUDIO_ALSA=1 才编译)
 *
 * 目标硬件 MAX98357A:I2S 从片 D 类功放 + 扬声器,RK3576 侧待 dts 使能
 * simple-audio-card 后即出现在 ALSA(默认卡);软件按标准 PCM 设备对待,
 * 48k/S16LE/双声道阻塞写。增益脚(GAIN)定硬件响度上限,细调走软件音量。
 *
 * 健壮性:underrun(-EPIPE)/挂起(-ESTRPIPE)走 snd_pcm_recover 续播;
 * 部分写回环补齐。硬件未接入时 snd_pcm_open 失败 = 播放器降级,不报障。
 */
#if defined(DG_AUDIO_ALSA) && DG_AUDIO_ALSA

#include "audio_hw.h"
#include "dg_log.h"

#include <alsa/asoundlib.h>

static const char *TAG = "[AUDIO]";

static snd_pcm_t *s_pcm;

static void alsa_close(void);

static int alsa_open(const char *device, unsigned rate, unsigned channels)
{
    if (s_pcm)                              /* 幂等:重复 open 先关旧流 */
        alsa_close();
    if (!device || !device[0])
        device = "default";

    int rc = snd_pcm_open(&s_pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        DG_LOGW(TAG, "snd_pcm_open(%s) 失败:%s(硬件未接入?降级静默)",
                device, snd_strerror(rc));
        s_pcm = NULL;
        return DG_ERR_IO;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    bool ok = snd_pcm_hw_params_any(s_pcm, hw) >= 0 &&
              snd_pcm_hw_params_set_access(s_pcm, hw,
                                           SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
              snd_pcm_hw_params_set_format(s_pcm, hw,
                                           SND_PCM_FORMAT_S16_LE) >= 0 &&
              snd_pcm_hw_params_set_channels(s_pcm, hw, channels) >= 0;
    unsigned r = rate;
    int dir = 0;
    ok = ok && snd_pcm_hw_params_set_rate_near(s_pcm, hw, &r, &dir) >= 0;
    if (!ok || r != rate) {                 /* 48k 都谈不下来的声卡不伺候 */
        DG_LOGW(TAG, "hw 参数协商失败(rate=%u got=%u)", rate, r);
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return DG_ERR_UNSUPPORTED;
    }
    snd_pcm_uframes_t period = 960;         /* 20ms @48k:提示音延迟与稳态折中 */
    dir = 0;
    (void)snd_pcm_hw_params_set_period_size_near(s_pcm, hw, &period, &dir);
    if (snd_pcm_hw_params(s_pcm, hw) < 0 ||
        snd_pcm_prepare(s_pcm) < 0) {
        DG_LOGW(TAG, "hw 参数提交/prepare 失败");
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return DG_ERR_IO;
    }
    DG_LOGI(TAG, "ALSA 输出就绪(%s %uHz %uch S16LE)", device, rate, channels);
    return DG_OK;
}

static int alsa_write(const int16_t *frames, size_t n_frames)
{
    if (!s_pcm || !frames || !n_frames)
        return DG_ERR_PARAM;
    const int16_t *p = frames;
    size_t left = n_frames;
    while (left > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(s_pcm, p, left);
        if (w < 0) {
            w = snd_pcm_recover(s_pcm, (int)w, 1);   /* underrun/挂起续播 */
            if (w < 0) {
                DG_LOGW(TAG, "PCM 写失败:%s", snd_strerror((int)w));
                return DG_ERR_IO;
            }
            continue;
        }
        p += (size_t)w * 2;                 /* 立体声 = 2 采样/帧 */
        left -= (size_t)w;
    }
    return DG_OK;
}

static void alsa_close(void)
{
    if (s_pcm) {
        snd_pcm_drain(s_pcm);
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
    }
}

const audio_hw_ops_t audio_hw_alsa_ops = {
    .name = "alsa",
    .open = alsa_open,
    .write = alsa_write,
    .close = alsa_close,
};

#endif /* DG_AUDIO_ALSA */
