/*
 * test_audio.c — 语音播报模块宿主测试(modules/audio;sink 后端注入)
 *
 * 覆盖:WAV 头解析判定表(格式/位深/声道/截断)、正弦渲染(静音/相位
 * 连续性/幅度域)、播放器(sink 落帧数=时长×48k、淡入出首帧近零、音量
 * 幅度域、单声道扩展、错采样率拒播、命名提示音命中与缺席、开门/拒绝
 * 事件自动提示、PASS 不触发、心跳活性、幂等停服)。
 *
 * cfg 注入:prompt_dir 是 json-only 键,本测试写一份临时 default.json
 * 把 audio.prompt_dir 指到测试目录(顺带覆盖新键的 json 加载路径)。
 */
#include "dg_test.h"

#include "audio/audio_hw_sink.h"
#include "audio/audio_player.h"
#include "cfg.h"
#include "event_bus.h"
#include "events.h"
#include "audio/tone.h"
#include "audio/wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- 测试用 WAV 组包(小端手工拼,不依赖外部素材) ---- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* PCM16 wav 组包;fmt 可注入异常值(0x11=ADPCM / bits=24 / ch=3 做负例) */
typedef struct {
    uint8_t *buf;
    size_t len;
} wav_blob_t;

static wav_blob_t mk_wav(unsigned rate, unsigned ch, unsigned bits,
                         uint16_t fmt, size_t frames, int16_t level)
{
    const size_t data_len = frames * ch * 2;
    const size_t len = 44 + data_len;
    uint8_t *b = calloc(1, len ? len : 1);
    memcpy(b, "RIFF", 4);
    put32(b + 4, (uint32_t)(len - 8));
    memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4);
    put32(b + 16, 16);
    put16(b + 20, fmt);
    put16(b + 22, (uint16_t)ch);
    put32(b + 24, rate);
    put32(b + 28, rate * ch * 2);
    put16(b + 32, (uint16_t)(ch * 2));
    put16(b + 34, (uint16_t)bits);
    memcpy(b + 36, "data", 4);
    put32(b + 40, (uint32_t)data_len);
    for (size_t i = 0; i < frames * ch; i++)
        put16(b + 44 + i * 2, (uint16_t)(i & 1 ? level : -level));
    return (wav_blob_t){ .buf = b, .len = len };
}

static void t_wav_probe(void)
{
    printf("[A1] WAV 头解析判定表\n");
    wav_info_t wi;

    wav_blob_t ok = mk_wav(48000, 2, 16, 1, 100, 1000);
    DG_CHECK(wav_probe(ok.buf, ok.len, &wi) == DG_OK);
    DG_CHECK(wi.rate == 48000 && wi.channels == 2 && wi.bits == 16);
    DG_CHECK(wi.data_off == 44 && wi.data_len == 100 * 2 * 2);
    free(ok.buf);

    wav_blob_t mono = mk_wav(48000, 1, 16, 1, 50, 500);
    DG_CHECK(wav_probe(mono.buf, mono.len, &wi) == DG_OK && wi.channels == 1);
    free(mono.buf);

    wav_blob_t adpcm = mk_wav(48000, 2, 16, 0x11, 10, 0);   /* 压缩格式拒收 */
    DG_CHECK(wav_probe(adpcm.buf, adpcm.len, &wi) == DG_ERR_UNSUPPORTED);
    free(adpcm.buf);

    wav_blob_t bits24 = mk_wav(48000, 2, 24, 1, 10, 0);     /* 非 16bit 拒收 */
    DG_CHECK(wav_probe(bits24.buf, bits24.len, &wi) == DG_ERR_UNSUPPORTED);
    free(bits24.buf);

    wav_blob_t ch3 = mk_wav(48000, 3, 16, 1, 10, 0);        /* 3 声道拒收 */
    DG_CHECK(wav_probe(ch3.buf, ch3.len, &wi) == DG_ERR_UNSUPPORTED);
    free(ch3.buf);

    uint8_t notriff[64] = { 0 };
    DG_CHECK(wav_probe(notriff, sizeof(notriff), &wi) == DG_ERR_IO);

    wav_blob_t cut = mk_wav(48000, 2, 16, 1, 100, 0);       /* data 截断 */
    DG_CHECK(wav_probe(cut.buf, cut.len - 40, &wi) == DG_ERR_IO);
    free(cut.buf);

    DG_CHECK(wav_probe(NULL, 0, &wi) == DG_ERR_PARAM);
}

static void t_tone_render(void)
{
    printf("[A2] 正弦渲染:静音/相位连续/幅度域\n");

    int16_t mute[480];
    tone_render(mute, 240, 48000, 2, 0.0, 1.0, NULL);       /* freq 0 = 静音 */
    int zeros = 0;
    for (int i = 0; i < 480; i++)
        zeros += (mute[i] == 0);
    DG_CHECK(zeros == 480);

    /* 分两段渲染(相位携带)与一次整段渲染逐样本一致 = 跨 chunk 不断波 */
    int16_t whole[960];                                     /* 480 帧立体声 */
    double ph = 0.0;
    tone_render(whole, 480, 48000, 2, 1000.0, 0.5, &ph);
    DG_CHECK(ph >= 0.0 && ph < 2.0 * M_PI);

    int16_t split[960];
    double ph2 = 0.0;
    tone_render(split, 200, 48000, 2, 1000.0, 0.5, &ph2);
    tone_render(split + 400, 280, 48000, 2, 1000.0, 0.5, &ph2);
    int match = 1;
    for (int i = 0; i < 960; i++)
        if (split[i] != whole[i])
            match = 0;
    DG_CHECK(match == 1);

    int peak = 0;
    for (int i = 0; i < 960; i++) {
        int v = abs(whole[i]);
        if (v > peak)
            peak = v;
    }
    /* vol=0.5、幅度上限 0.95×32767 → 峰值域 (5000, 17000] */
    DG_CHECK(peak > 5000 && peak <= 17000);
}

static char s_dir[160];

static void write_prompt(const char *name, unsigned rate, unsigned ch)
{
    char path[384];   /* s_dir(159)+文件名,防 -Wformat-truncation */
    snprintf(path, sizeof(path), "%s/%s.wav", s_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    wav_blob_t w = mk_wav(rate, ch, 16, 1, 480, 12000);     /* 10ms */
    fwrite(w.buf, 1, w.len, f);
    fclose(f);
    free(w.buf);
}

static void t_player(void)
{
    printf("[A3] 播放器:sink 落帧/淡入出/单声道/拒错率/命名提示/事件挂接\n");

    /* default.json 只注入 audio.prompt_dir(json-only 键加载路径顺带覆盖) */
    char def[192];
    snprintf(def, sizeof(def), "%s/default.json", s_dir);
    FILE *f = fopen(def, "wb");
    DG_CHECK(f != NULL);
    if (f) {
        fprintf(f, "{\"audio\":{\"prompt_dir\":\"%s\"}}", s_dir);
        fclose(f);
    }
    char cur[192];
    snprintf(cur, sizeof(cur), "%s/cur.json", s_dir);
    f = fopen(cur, "wb");
    DG_CHECK(f != NULL);
    if (f) {
        fprintf(f, "{}");
        fclose(f);
    }
    DG_CHECK(cfg_load(def, cur) == DG_OK);
    DG_CHECK(strcmp(cfg_get()->audio_prompt_dir, s_dir) == 0);
    DG_CHECK(cfg_get()->audio_volume == 80);                /* 内置默认 */
    DG_CHECK(cfg_get()->audio_enabled == 1);

    audio_hw_attach(&audio_sink_ops);
    DG_CHECK(audio_player_start() == DG_OK);
    DG_CHECK(audio_player_start() == DG_OK);                /* 幂等 */
    audio_player_test_wait_idle(1000);

    /* 100ms 提示音 → 恰 4800 帧;首帧淡入近零;峰值带音量(80%) */
    audio_sink_reset();
    DG_CHECK(audio_play_tone(1000.0, 100) == DG_OK);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 4800);
    int16_t l, r;
    DG_CHECK(audio_sink_frame(0, &l, &r) && abs(l) < 800);  /* 8ms 淡入起坡 */
    int peak = audio_sink_peak();
    DG_CHECK(peak > 4000 && peak < 26000);                  /* 0.95×32767×0.8≈24900 */
    DG_CHECK(audio_player_heartbeat_ms() > 0);

    /* 参数域:0/超长时长拒绝 */
    DG_CHECK(audio_play_tone(1000.0, 0) == DG_ERR_PARAM);
    DG_CHECK(audio_play_tone(1000.0, 20000) == DG_ERR_PARAM);

    /* 立体声 WAV:10ms = 480 帧 */
    char path[384];   /* s_dir(159)+文件名,防 -Wformat-truncation */
    snprintf(path, sizeof(path), "%s/st.wav", s_dir);
    f = fopen(path, "wb");
    wav_blob_t st = mk_wav(48000, 2, 16, 1, 480, 12000);
    fwrite(st.buf, 1, st.len, f);
    fclose(f);
    free(st.buf);
    audio_sink_reset();
    DG_CHECK(audio_play_file(path) == DG_OK);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 480);

    /* 单声道 WAV:左右声道同值 */
    snprintf(path, sizeof(path), "%s/mo.wav", s_dir);
    f = fopen(path, "wb");
    wav_blob_t mo = mk_wav(48000, 1, 16, 1, 480, 12000);
    fwrite(mo.buf, 1, mo.len, f);
    fclose(f);
    free(mo.buf);
    audio_sink_reset();
    DG_CHECK(audio_play_file(path) == DG_OK);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 480);
    int mono_same = 1;
    for (size_t i = 0; i < 480; i++) {
        if (!audio_sink_frame(i, &l, &r) || l != r)
            mono_same = 0;
    }
    DG_CHECK(mono_same == 1);

    /* 44.1k 素材:拒播(README 钉死 48k),帧数不动 */
    snprintf(path, sizeof(path), "%s/bad.wav", s_dir);
    f = fopen(path, "wb");
    wav_blob_t bad = mk_wav(44100, 2, 16, 1, 480, 12000);
    fwrite(bad.buf, 1, bad.len, f);
    fclose(f);
    free(bad.buf);
    audio_sink_reset();
    DG_CHECK(audio_play_file(path) == DG_OK);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 0);

    /* 命名提示:命中 → OK;缺席名 → NOT_FOUND */
    write_prompt("success", 48000, 2);
    DG_CHECK(audio_prompt_play("success") == DG_OK);
    DG_CHECK(audio_prompt_play("nope") == DG_ERR_NOT_FOUND);
    audio_player_test_wait_idle(2000);

    /* 事件挂接:开门 → success.wav(480 帧);拒绝 → fail.wav;PASS 不触发 */
    write_prompt("fail", 48000, 1);
    audio_sink_reset();
    EVENT_BUS_PUBLISH_EMPTY(EV_AUTH_DOOR_OPEN);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 480);

    audio_sink_reset();
    ev_auth_result_t rej;
    memset(&rej, 0, sizeof(rej));
    rej.has_user = false;
    rej.result = DG_RESULT_REJECT;
    rej.method = DG_METHOD_PWD;
    EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &rej);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 480);                   /* fail.wav 单声道 480 帧 */

    audio_sink_reset();
    rej.result = DG_RESULT_PASS;
    EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &rej);
    audio_player_test_wait_idle(2000);
    DG_CHECK(audio_sink_frames() == 0);

    audio_player_stop();
    audio_player_stop();                                    /* 幂等 */
    DG_CHECK(audio_player_ready() == false);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);

    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_audio_%d", (int)getpid());
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    DG_CHECK(system(cmd) == 0);

    t_wav_probe();
    t_tone_render();
    t_player();

    char rm[256];
    snprintf(rm, sizeof(rm), "rm -rf %s", s_dir);
    (void)system(rm);
    DG_TEST_EXIT();
}
