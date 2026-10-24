/*
 * web_stream.c — web 监控推流实现(架构 v2 决议:RTSP 暂缓,WS 快照)
 *
 * 链路:worker 线程 10fps 节拍拉 camera_latest()(4B/像素,内存字节序
 * BGRA8888——板上=720×1280 竖屏,板侧转换经 camera_stream_preview_set
 * 按位或打开;sim 恒出帧)
 * → 等比降采样 + XRGB→RGB888 → dg_jpeg 编码 → 发布缓冲 → netcore_post 通知
 * loop 线程发送。
 *
 * 线程契约:连接只在 loop 线程被碰(mg_ws_send 在 web_server 的回调里);
 * 本文件只有一份发布缓冲,worker 写入与 loop 读出共用一把锁,拷贝都在锁内
 * 完成(40KB 级 memcpy,微秒级占用)。相机三缓冲的读窗口是 3 个帧周期
 * (~100ms@30fps),我们的降采样直读只要 1~3ms,余量充足,无需重验 seq。
 */
#define _GNU_SOURCE  /* pthread_cond_clockwait:等待时钟切 MONOTONIC */
#include "web_stream.h"

#include "camera.h"
#include "cfg.h"
#include "dg_jpeg.h"
#include "dg_log.h"
#include "err.h"
#include "timeutil.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[STREAM]";

#define STREAM_FPS        10          /* 用户拍板:固定 10fps,不做档位 */
#define STREAM_PERIOD_MS  (1000 / STREAM_FPS)
#define STREAM_VIEWER_MAX 4

static pthread_t s_thread;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static bool s_running = false;        /* 线程生命周期(worker 自检退出) */
static bool s_enabled = false;        /* 配置 web.video_enabled */
static int s_viewers = 0;
static int s_max_side = 640;
static int s_quality = 70;

/* 发布缓冲(锁内读写)+ worker 私有暂存(无锁) */
static uint8_t s_pub[DG_WEB_STREAM_JPEG_CAP];
static size_t s_pub_len = 0;
static uint32_t s_pub_seq = 0;        /* 已发布帧序号(消费者据此判断新帧) */
static uint8_t *s_rgb;                /* 降采样后 RGB888(启动按 max_side 分配) */
static uint8_t *s_enc;                /* JPEG 编码暂存 */
static uint32_t s_last_src_seq;       /* 上次编码过的相机帧 seq(重复帧跳过) */
static bool s_claimed;                /* 当前是否持有相机取帧开关 */

/* 等比降采样 + XRGB8888→RGB888。目标长边 = min(源长边, s_max_side);
 * 用整数盒滤波(源盒平均),缩放比任意时抗混叠,整倍率时退化为直采 */
static void convert_frame(const uint8_t *xrgb, int sw, int sh,
                          int *dw, int *dh)
{
    int long_side = sw > sh ? sw : sh;
    int target = long_side > s_max_side ? s_max_side : long_side;
    int w = (int)((int64_t)sw * target / long_side);
    int h = (int)((int64_t)sh * target / long_side);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    *dw = w;
    *dh = h;

    for (int y = 0; y < h; y++) {
        int sy0 = (int)((int64_t)y * sh / h);
        int sy1 = (int)((int64_t)(y + 1) * sh / h);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        uint8_t *out = s_rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            int sx0 = (int)((int64_t)x * sw / w);
            int sx1 = (int)((int64_t)(x + 1) * sw / w);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0;
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *p = xrgb + ((size_t)sy * sw + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++, p += 4) {
                    /* 帧内存字节序 = BGRA8888(见 camera.h camera_frame_t;
                     * 板上 RGA 用 RK_FORMAT_BGRA_8888 写出,与 LVGL 32 位
                     * 直通一致)。首版按 XRGB 读反,整画面紫屏(2026-10-14) */
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    n++;
                }
            }
            out[x * 3 + 0] = (uint8_t)(r / n);
            out[x * 3 + 1] = (uint8_t)(g / n);
            out[x * 3 + 2] = (uint8_t)(b / n);
        }
    }
}

/* 单拍工作:取帧→降采样→编码→发布(无新帧直接返回,零成本) */
static void do_tick(void)
{
    if (!s_claimed) {
        camera_stream_preview_set(true);
        s_claimed = true;
        DG_LOGI(TAG, "开始推流取帧(%dfps,长边<=%d,质量 %d)",
                STREAM_FPS, s_max_side, s_quality);
    }

    const camera_frame_t *f = camera_latest();
    /* 无帧/重复帧/未就绪的 0×0 帧(rot=0/180 输出缓冲未分配时):跳过——
     * pixels 与宽高都要判,下游 dg_jpeg_encode_rgb 不防 0 尺寸(B5-2) */
    if (!f || !f->pixels || f->w == 0 || f->h == 0 || f->seq == 0 ||
        f->seq == s_last_src_seq)
        return;
    s_last_src_seq = f->seq;

    int dw, dh;
    convert_frame(f->pixels, f->w, f->h, &dw, &dh);
    size_t len = 0;
    if (dg_jpeg_encode_rgb(s_rgb, dw, dh, s_quality,
                           s_enc, DG_WEB_STREAM_JPEG_CAP, &len) != DG_OK) {
        DG_LOGW(TAG, "JPEG 编码失败(%dx%d)", dw, dh);
        return;
    }

    pthread_mutex_lock(&s_mtx);
    memcpy(s_pub, s_enc, len);
    s_pub_len = len;
    s_pub_seq++;
    pthread_mutex_unlock(&s_mtx);
    /* 发送侧(web_server 的 loop 定时器)按 s_pub_seq 变化自行拉取,
     * 这里不投递——worker 与 netcore 无反向依赖 */
}

/* 睡到截止时刻(now_mono_ms 单调域;被调度惊醒则继续睡) */
static void sleep_to(int64_t deadline)
{
    while (true) {
        int64_t now = now_mono_ms();
        if (now >= deadline)
            return;
        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = (long)(deadline - now) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
}

/* 停车:无人订阅时线程睡在条件变量上(用户拍板"平时不跑"),零唤醒零相机
 * 开销;订阅/退订/停服各经 broadcast 唤醒重判。超时 1s 只是 quit 兜底,
 * 正常增减订都走即时唤醒。返回 true = 停服退出 */
static bool park_until_viewer_or_quit(void)
{
    pthread_mutex_lock(&s_mtx);
    while (s_running && s_viewers <= 0) {
        if (s_claimed) {
            camera_stream_preview_set(false);    /* 顺手释放取帧(纯标志写) */
            s_claimed = false;
            DG_LOGI(TAG, "无订阅者,推流取帧已停");
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += 1;                          /* 兜底周期,正常不走到 */
        pthread_cond_clockwait(&s_cv, &s_mtx, CLOCK_MONOTONIC, &ts);
    }
    bool quit = !s_running;
    pthread_mutex_unlock(&s_mtx);
    return quit;
}

static void *worker(void *arg)
{
    (void)arg;
    while (true) {
        if (park_until_viewer_or_quit())
            break;

        /* 截止时刻先于工作确定:转换+编码耗时(板上实测 ~15ms/帧)不挤占
         * 帧率——"固定 10fps"才名副其实(固定睡 100ms 会退化成 ~8.5fps) */
        int64_t deadline = now_mono_ms() + STREAM_PERIOD_MS;
        do_tick();
        sleep_to(deadline);
    }
    if (s_claimed)
        camera_stream_preview_set(false);
    return NULL;
}

int web_stream_start(void)
{
    pthread_mutex_lock(&s_mtx);
    if (s_running) {
        pthread_mutex_unlock(&s_mtx);
        return DG_OK;
    }
    const dg_cfg_t *cfg = cfg_get();
    s_enabled = cfg && cfg->video_enabled;
    s_max_side = cfg ? cfg->video_max_side : 640;
    s_quality = cfg ? cfg->video_quality : 70;
    if (!s_enabled) {
        DG_LOGI(TAG, "监控推流已按配置屏蔽(web.video_enabled=0)");
        pthread_mutex_unlock(&s_mtx);
        return DG_OK;                    /* 空转启动:订阅请求按 DISABLED 拒 */
    }
    s_rgb = malloc((size_t)s_max_side * s_max_side * 3);
    s_enc = malloc(DG_WEB_STREAM_JPEG_CAP);
    if (!s_rgb || !s_enc) {
        free(s_rgb);
        free(s_enc);
        s_rgb = s_enc = NULL;
        pthread_mutex_unlock(&s_mtx);
        DG_LOGE(TAG, "编码缓冲分配失败,推流不可用");
        return DG_ERR_NO_MEMORY;
    }
    s_running = true;
    s_viewers = 0;
    s_pub_len = 0;
    s_pub_seq = 0;
    s_last_src_seq = 0;
    s_claimed = false;
    int rc = pthread_create(&s_thread, NULL, worker, NULL);
    if (rc != 0) {
        s_running = false;
        free(s_rgb);
        free(s_enc);
        s_rgb = s_enc = NULL;
        pthread_mutex_unlock(&s_mtx);
        DG_LOGE(TAG, "编码线程创建失败 rc=%d", rc);
        return DG_ERR_INTERNAL;
    }
    pthread_mutex_unlock(&s_mtx);
    DG_LOGI(TAG, "监控推流服务就绪(配置长边<=%d 质量 %d)", s_max_side, s_quality);
    return DG_OK;
}

void web_stream_stop(void)
{
    pthread_mutex_lock(&s_mtx);
    if (!s_running) {
        pthread_mutex_unlock(&s_mtx);
        return;
    }
    s_running = false;
    s_viewers = 0;
    pthread_cond_broadcast(&s_cv);           /* 唤醒停车中的 worker 走退出分支 */
    pthread_mutex_unlock(&s_mtx);
    pthread_join(s_thread, NULL);
    free(s_rgb);
    free(s_enc);
    s_rgb = s_enc = NULL;
    DG_LOGI(TAG, "监控推流服务已停");
}

bool web_stream_available(void)
{
    pthread_mutex_lock(&s_mtx);
    bool ok = s_running && s_enabled;
    pthread_mutex_unlock(&s_mtx);
    return ok;
}

int web_stream_viewer_add(void)
{
    pthread_mutex_lock(&s_mtx);
    if (!s_running || !s_enabled) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_UNSUPPORTED;   /* 配置关闭:能力不可用(语义同 err.h) */
    }
    if (s_viewers >= STREAM_VIEWER_MAX) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_BUSY;
    }
    s_viewers++;
    int n = s_viewers;
    pthread_cond_broadcast(&s_cv);           /* 唤醒停车中的 worker 开始取帧 */
    pthread_mutex_unlock(&s_mtx);
    if (n == 1)
        DG_LOGI(TAG, "web 客户端订阅监控画面");
    return DG_OK;
}

void web_stream_viewer_remove(void)
{
    pthread_mutex_lock(&s_mtx);
    if (s_viewers > 0)
        s_viewers--;
    pthread_cond_broadcast(&s_cv);           /* 令 worker 尽快停车释放取帧 */
    pthread_mutex_unlock(&s_mtx);
}

bool web_stream_active(void)
{
    pthread_mutex_lock(&s_mtx);
    bool on = s_running && s_viewers > 0;
    pthread_mutex_unlock(&s_mtx);
    return on;
}

bool web_stream_snapshot(uint8_t *dst, size_t cap, size_t *len, uint32_t *seq)
{
    pthread_mutex_lock(&s_mtx);
    if (s_pub_len == 0 || s_pub_len > cap) {
        pthread_mutex_unlock(&s_mtx);
        return false;
    }
    memcpy(dst, s_pub, s_pub_len);
    if (len)
        *len = s_pub_len;
    if (seq)
        *seq = s_pub_seq;
    pthread_mutex_unlock(&s_mtx);
    return true;
}
