/*
 * audio_player.c — 播放器实现:作业队列 + 单播放线程 + 两级降级
 *
 * 播放线程私有:后端生命周期(懒重试)、逐 chunk 渲染(20ms);
 * 心跳每 chunk 刷新。envelope(首尾 8ms 线性淡入出)在写前统一施加——
 * 提示音突起突止在功放上是明显的「咔」声。
 */
#include "audio_player.h"
#include "audio_hw.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "timeutil.h"
#include "tone.h"
#include "wav.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[AUDIO]";

#define AUDIO_RATE        48000
#define AUDIO_CHANNELS    2
#define AUDIO_CHUNK_FR    960                 /* 20ms/块:提示延迟与开销折中 */
#define AUDIO_QUEUE_MAX   4
#define AUDIO_FADE_FR     ((AUDIO_RATE / 1000) * 8)   /* 8ms 淡入出 */
#define AUDIO_RETRY_MS    5000                /* 后端懒重试周期 */
#define AUDIO_WAV_MAX     (2 * 1024 * 1024)   /* 单文件上限:语音提示用不到 */

/* ---- 状态 ---- */

static atomic_bool s_running = false;
static volatile bool s_ready;                 /* 后端打开成功 */
static bool s_degrade_pub;                    /* 降级事件只发一次(恢复重置) */
static atomic_llong s_hb_ms;
static int64_t s_next_open_ms;                /* 降级后的下次重试时刻 */

static pthread_t s_tid;
static bool s_tid_created;

typedef enum { JOB_TONE = 0, JOB_FILE = 1 } job_kind_t;
typedef struct {
    job_kind_t kind;
    double freq_hz;
    uint32_t dur_ms;
    char path[128];
} audio_job_t;

static audio_job_t s_q[AUDIO_QUEUE_MAX];
static int s_q_head = 0, s_q_tail = 0, s_q_dropped = 0;
static pthread_mutex_t s_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cond = PTHREAD_COND_INITIALIZER;

static event_subscription_t *s_sub_open;
static event_subscription_t *s_sub_reject;

/* ---- 事件(总线线程) ---- */

static void pub_state(bool ready)
{
    ev_audio_state_t ev = { .ready = ready };
    EVENT_BUS_PUBLISH(EV_AUDIO_STATE, &ev);
}

/* ---- 后端(播放线程) ---- */

static const audio_hw_ops_t *hw(void)
{
    return audio_hw_current();
}

static bool backend_ensure(void)
{
    if (s_ready)
        return true;
    if (!cfg_get()->audio_enabled)
        return false;
    const audio_hw_ops_t *o = hw();
    if (!o)
        return false;
    const int64_t now = now_mono_ms();
    if (now < s_next_open_ms)
        return false;
    s_next_open_ms = now + AUDIO_RETRY_MS;
    if (o->open(cfg_get()->audio_device, AUDIO_RATE, AUDIO_CHANNELS) == DG_OK) {
        s_ready = true;
        if (s_degrade_pub) {
            s_degrade_pub = false;
            pub_state(true);
            DG_LOGI(TAG, "音频后端恢复(%s)", o->name);
        }
        return true;
    }
    if (!s_degrade_pub) {
        s_degrade_pub = true;
        pub_state(false);
        DG_LOGW(TAG, "音频后端不可用,降级静默(每 %ds 重试)", AUDIO_RETRY_MS / 1000);
    }
    return false;
}

/* ---- 渲染原语(播放线程) ---- */

/* 音量+淡入出包络:src(立体声交错)→ dst;total_frames = 本作业总帧数,
 * pos = 当前 chunk 首帧绝对号;mono=1 时 src 单声道按 L 扩展 */
static void chunk_apply(int16_t *dst, const int16_t *src_l, const int16_t *src_r,
                        size_t n, int64_t pos, int64_t total, int vol_q15)
{
    for (size_t i = 0; i < n; i++) {
        int64_t t = pos + (int64_t)i;
        int env = 32767;
        if (t < AUDIO_FADE_FR)
            env = (int)(32767 * t / AUDIO_FADE_FR);
        else if (total - t < AUDIO_FADE_FR)
            env = (int)(32767 * (total - t) / AUDIO_FADE_FR);
        if (env < 0)
            env = 0;
        int32_t l = (int32_t)((int64_t)src_l[i] * vol_q15 >> 15) * env >> 15;
        int32_t r = (int32_t)((int64_t)src_r[i] * vol_q15 >> 15) * env >> 15;
        dst[i * 2] = (int16_t)l;
        dst[i * 2 + 1] = (int16_t)r;
    }
}

static void play_tone_job(double freq_hz, uint32_t dur_ms, int vol_q15)
{
    if (dur_ms == 0)
        dur_ms = 1;
    const int64_t total = (int64_t)AUDIO_RATE * dur_ms / 1000;
    double phase = 0.0;
    static int16_t tone[AUDIO_CHUNK_FR * AUDIO_CHANNELS];  /* 播放线程独占 */
    int16_t out[AUDIO_CHUNK_FR * AUDIO_CHANNELS];
    for (int64_t pos = 0; pos < total;) {
        s_hb_ms = now_mono_ms();
        const size_t n = (size_t)((total - pos) < AUDIO_CHUNK_FR
                                      ? (total - pos) : AUDIO_CHUNK_FR);
        tone_render(tone, n, AUDIO_RATE, AUDIO_CHANNELS, freq_hz, 1.0, &phase);
        chunk_apply(out, tone, tone, n, pos, total, vol_q15);
        if (hw()->write(out, n) != DG_OK)
            return;                           /* 写败:后端下次 ensure 重开 */
        pos += (int64_t)n;
    }
}

static void play_file_job(const char *path, int vol_q15)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        DG_LOGW(TAG, "提示音文件打不开:%s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > AUDIO_WAV_MAX) {
        DG_LOGW(TAG, "提示音文件尺寸异常(%ld):%s", sz, path);
        fclose(f);
        return;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        DG_LOGW(TAG, "提示音文件读不全:%s", path);
        return;
    }
    fclose(f);

    wav_info_t wi;
    if (wav_probe(buf, (size_t)sz, &wi) != DG_OK) {
        DG_LOGW(TAG, "提示音不是合法 WAV(48k/16bit/单双声道):%s", path);
        free(buf);
        return;
    }
    if (wi.rate != AUDIO_RATE) {
        /* 不做重采样:规格在 README 钉死,素材产出方保证;混速率播放
         * 出变声提示,宁可拒播 */
        DG_LOGW(TAG, "提示音采样率 %u ≠ %u,拒播:%s", wi.rate, AUDIO_RATE, path);
        free(buf);
        return;
    }

    const int16_t *pcm = (const int16_t *)(buf + wi.data_off);
    const int64_t total = (int64_t)(wi.data_len / (2 * wi.channels));
    int16_t out[AUDIO_CHUNK_FR * AUDIO_CHANNELS];
    for (int64_t pos = 0; pos < total;) {
        s_hb_ms = now_mono_ms();
        const size_t n = (size_t)((total - pos) < AUDIO_CHUNK_FR
                                      ? (total - pos) : AUDIO_CHUNK_FR);
        if (wi.channels == 2)
            chunk_apply(out, pcm + pos * 2, pcm + pos * 2 + 1, n, pos, total,
                        vol_q15);
        else                                  /* 单声道:左右同源 */
            chunk_apply(out, pcm + pos, pcm + pos, n, pos, total, vol_q15);
        if (hw()->write(out, n) != DG_OK)
            break;
        pos += (int64_t)n;
    }
    free(buf);
}

/* ---- 播放线程 ---- */

static void job_exec(const audio_job_t *j)
{
    if (!backend_ensure())
        return;                               /* 降级:静默跳过本作业 */
    const int vol_q15 = cfg_get()->audio_volume * 32767 / 100;
    if (j->kind == JOB_TONE)
        play_tone_job(j->freq_hz, j->dur_ms, vol_q15);
    else
        play_file_job(j->path, vol_q15);
}

static void *player_thread(void *arg)
{
    (void)arg;
    s_hb_ms = now_mono_ms();
    while (atomic_load(&s_running)) {
        pthread_mutex_lock(&s_q_mtx);
        while (atomic_load(&s_running) && s_q_head == s_q_tail)
            pthread_cond_wait(&s_q_cond, &s_q_mtx);
        if (!atomic_load(&s_running)) {
            pthread_mutex_unlock(&s_q_mtx);
            break;
        }
        audio_job_t j = s_q[s_q_tail];
        s_q_tail = (s_q_tail + 1) % AUDIO_QUEUE_MAX;
        pthread_mutex_unlock(&s_q_mtx);

        job_exec(&j);
        s_hb_ms = now_mono_ms();
    }
    /* 收尾:排空余量留给调用方(stop)关流 */
    return NULL;
}

/* ---- 队列(任意线程) ---- */

static void job_push(const audio_job_t *j)
{
    pthread_mutex_lock(&s_q_mtx);
    const int next = (s_q_head + 1) % AUDIO_QUEUE_MAX;
    if (next == s_q_tail) {                  /* 满:丢新(迟到的提示不如不响) */
        s_q_dropped++;
        pthread_mutex_unlock(&s_q_mtx);
        return;
    }
    s_q[s_q_head] = *j;
    s_q_head = next;
    pthread_cond_signal(&s_q_cond);
    pthread_mutex_unlock(&s_q_mtx);
}

static void wait_idle(int timeout_ms)
{
    /* 仅测试观察用:等队列排空(公共 API 不暴露,免得有人拿它当同步语义) */
    for (int i = 0; i < timeout_ms / 10; i++) {
        pthread_mutex_lock(&s_q_mtx);
        const bool idle = s_q_head == s_q_tail;
        pthread_mutex_unlock(&s_q_mtx);
        if (idle)
            return;
        usleep(10 * 1000);
    }
}
void audio_player_test_wait_idle(int timeout_ms)
{
    wait_idle(timeout_ms);
}

/* ---- 提示策略(总线线程) ---- */

/* 开门生效 → success(素材缺席降级为上行双音) */
static int on_door_open(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    if (!cfg_get()->audio_enabled)
        return 0;
    if (audio_prompt_play("success") == DG_ERR_NOT_FOUND) {
        audio_play_tone(880.0, 100);
        audio_play_tone(1318.5, 160);
    }
    return 0;
}

/* 验证拒绝 → fail(素材缺席降级为低音) */
static int on_auth_reject(const event_t *e, void *ud)
{
    (void)ud;
    const ev_auth_result_t *r = (const ev_auth_result_t *)e->data;
    if (!cfg_get()->audio_enabled || r->result == DG_RESULT_PASS)
        return 0;
    if (audio_prompt_play("fail") == DG_ERR_NOT_FOUND)
        audio_play_tone(330.0, 250);
    return 0;
}

/* ---- 公共 API ---- */

int audio_player_start(void)
{
    if (atomic_load(&s_running))
        return DG_OK;
    if (!cfg_get()->audio_enabled) {
        DG_LOGI(TAG, "audio.enabled=0,语音播报关");
        return DG_OK;
    }
    const audio_hw_ops_t *o = audio_hw_current();
    if (!o) {
        DG_LOGI(TAG, "无音频后端(宿主构建),播放器空转");
        return DG_OK;                         /* 不算错:降级语义 */
    }

    atomic_store(&s_running, true);
    s_ready = false;
    s_next_open_ms = 0;
    s_sub_open = event_bus_subscribe(EV_AUTH_DOOR_OPEN, on_door_open, NULL);
    s_sub_reject = event_bus_subscribe(EV_AUTH_RESULT, on_auth_reject, NULL);
    if (pthread_create(&s_tid, NULL, player_thread, NULL) != 0) {
        atomic_store(&s_running, false);
        event_bus_unsubscribe(s_sub_open);
        event_bus_unsubscribe(s_sub_reject);
        s_sub_open = s_sub_reject = NULL;
        DG_LOGE(TAG, "播放线程创建失败");
        return DG_ERR_INTERNAL;
    }
    s_tid_created = true;
    DG_LOGI(TAG, "语音播报就绪(后端 %s,音量 %d%%,素材目录 %s)", o->name,
            cfg_get()->audio_volume, cfg_get()->audio_prompt_dir);
    return DG_OK;
}

void audio_player_stop(void)
{
    if (!atomic_load(&s_running))
        return;
    atomic_store(&s_running, false);
    pthread_mutex_lock(&s_q_mtx);
    pthread_cond_broadcast(&s_q_cond);
    pthread_mutex_unlock(&s_q_mtx);
    if (s_tid_created) {
        pthread_join(s_tid, NULL);
        s_tid_created = false;
    }
    if (s_sub_open) {
        event_bus_unsubscribe(s_sub_open);
        s_sub_open = NULL;
    }
    if (s_sub_reject) {
        event_bus_unsubscribe(s_sub_reject);
        s_sub_reject = NULL;
    }
    if (s_ready && hw()) {
        hw()->close();
        s_ready = false;
    }
    DG_LOGI(TAG, "语音播报停止(队列丢弃累计 %d)", s_q_dropped);
}

bool audio_player_ready(void)
{
    return s_ready;
}

int audio_play_tone(double freq_hz, uint32_t dur_ms)
{
    if (!atomic_load(&s_running))
        return DG_ERR_NOT_INIT;
    if (dur_ms == 0 || dur_ms > 10000)
        return DG_ERR_PARAM;
    audio_job_t j = { .kind = JOB_TONE, .freq_hz = freq_hz, .dur_ms = dur_ms };
    job_push(&j);
    return DG_OK;
}

int audio_play_file(const char *wav_path)
{
    if (!atomic_load(&s_running))
        return DG_ERR_NOT_INIT;
    if (!wav_path || !wav_path[0] || strlen(wav_path) >= sizeof(((audio_job_t *)0)->path))
        return DG_ERR_PARAM;
    audio_job_t j = { .kind = JOB_FILE };
    snprintf(j.path, sizeof(j.path), "%s", wav_path);
    job_push(&j);
    return DG_OK;
}

int audio_prompt_play(const char *name)
{
    if (!name || !name[0])
        return DG_ERR_PARAM;
    if (!atomic_load(&s_running))
        return DG_ERR_NOT_INIT;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.wav", cfg_get()->audio_prompt_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return DG_ERR_NOT_FOUND;
    fclose(f);
    return audio_play_file(path);
}

int64_t audio_player_heartbeat_ms(void)
{
    return s_hb_ms;
}
