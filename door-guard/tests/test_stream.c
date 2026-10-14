/*
 * test_stream.c — web 监控推流测试(2026-10-14)
 *
 * tests 模式不编 dg_camera(tests 分支在主 CMakeLists 的 return() 之前,
 * 该目标只在板/sim 模式定义),所以这里用相机桩:camera_latest 返回可控帧、
 * camera_stream_preview_set 记录取帧开关——顺带把「订阅开取帧/退订释放取帧」
 * 变成可断言行为。覆盖:编码发布(SOI/EOI/降采样尺寸)、10fps 节流上限、
 * 重复帧跳过、退订即停+取帧释放、配置关闭时订阅被拒。发送侧(WS 二进制)
 * 不在宿主范围,板上手验。
 */
#include "cfg.h"
#include "camera.h"
#include "dg_jpeg.h"
#include "dg_test.h"
#include "storage.h"
#include "web_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SRC_W 320
#define SRC_H 160              /* 长边 320,cfg max_side=160 → 期望 160×80 */
#define EXP_W 160
#define EXP_H 80

/* ---- 相机桩(tests 模式无 dg_camera,符号由本文件提供) ---- */

static uint8_t s_px[SRC_W * SRC_H * 4];      /* XRGB8888 */
static camera_frame_t s_frame;               /* seq=0 = 无帧 */
static int s_claim_calls;
static bool s_claim_on;

const camera_frame_t *camera_latest(void)
{
    return s_frame.seq ? &s_frame : NULL;
}

void camera_stream_preview_set(bool on)
{
    s_claim_calls++;
    s_claim_on = on;
}

/* 产生新帧:像素随计数变化(内容不影响断言,只保证非全零) */
static void frame_push(void)
{
    static uint8_t v;
    memset(s_px, ++v, sizeof(s_px));
    s_frame.w = SRC_W;
    s_frame.h = SRC_H;
    s_frame.seq++;
    s_frame.pixels = s_px;
}

/* 指定纯色推帧(字节序 = 帧标准 BGRA8888);通道序回归测试用 */
static void frame_push_color(uint8_t b, uint8_t g, uint8_t r)
{
    for (size_t i = 0; i < sizeof(s_px); i += 4) {
        s_px[i + 0] = b;
        s_px[i + 1] = g;
        s_px[i + 2] = r;
        s_px[i + 3] = 0xFF;
    }
    s_frame.w = SRC_W;
    s_frame.h = SRC_H;
    s_frame.seq++;
    s_frame.pixels = s_px;
}

static void msleep(int ms)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    nanosleep(&ts, NULL);
}

/* 等待条件成立(worker 100ms 节拍,留足余量)。条件以函数指针传入——
 * 按值传 bool 只在调用时求值一次,循环会一直检查过期副本(首轮实踩) */
static bool wait_for(bool (*cond)(void), int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 20) {
        if (cond())
            return true;
        msleep(20);
    }
    return cond();
}

static char s_dir[64], s_def[96], s_cur[96];

static void setup_env(int enabled)
{
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_stream_%d", (int)getpid());
    snprintf(s_def, sizeof(s_def), "%s/default.json", s_dir);
    snprintf(s_cur, sizeof(s_cur), "%s/cur_config.json", s_dir);
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        exit(1);

    char db[96], key[96];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    storage_deinit();
    if (storage_init(db, key) != DG_OK)
        exit(1);

    FILE *f = fopen(s_def, "wb");
    if (!f)
        exit(1);
    fprintf(f,
            "{\"network\":{\"web_port\":8080,\"video_enabled\":%s,"
            "\"video_max_side\":160,\"video_quality\":70}}",
            enabled ? "true" : "false");
    fclose(f);
    f = fopen(s_cur, "wb");
    if (!f)
        exit(1);
    fprintf(f, "{}");
    fclose(f);

    if (cfg_load(s_def, s_cur) != DG_OK)
        exit(1);
    s_frame.seq = 0;
    s_claim_calls = 0;
    s_claim_on = false;
}

static uint8_t s_jpeg[256 * 1024];

static bool claimed_on(void)
{
    return s_claim_on;
}

static bool claimed_off(void)
{
    return !s_claim_on;
}

static bool grab(size_t *len, uint32_t *seq)
{
    return web_stream_snapshot(s_jpeg, sizeof(s_jpeg), len, seq);
}

/* 持续 n_ms 毫秒按 ~15fps 推帧,统计快照出现的不同帧数 */
static int collect_frames(int n_ms, uint32_t *last_seq)
{
    int count = 0;
    uint32_t seen = 0;
    for (int t = 0; t < n_ms; t += 66) {
        frame_push();
        msleep(66);
        size_t len = 0;
        uint32_t seq = 0;
        if (grab(&len, &seq) && seq != 0 && seq != seen) {
            seen = seq;
            count++;
        }
    }
    if (last_seq)
        *last_seq = seen;
    return count;
}

static void test_happy_path(void)
{
    DG_CHECK(web_stream_start() == DG_OK);
    DG_CHECK(web_stream_available() == true);
    DG_CHECK(web_stream_viewer_add() == DG_OK);
    DG_CHECK(wait_for(claimed_on, 1000));      /* 订阅后取帧开关被打开 */

    uint32_t last = 0;
    int n = collect_frames(1200, &last);
    DG_CHECK(n >= 2);                          /* 10fps × 1.2s,下限放宽防慢机 */
    DG_CHECK(n <= 15);                         /* 上限=节流生效(源 15fps 不设限约 18) */

    size_t len = 0;
    uint32_t seq = 0;
    DG_CHECK(grab(&len, &seq));
    DG_CHECK(len > 2 && len <= sizeof(s_jpeg));
    DG_CHECK(s_jpeg[0] == 0xFF && s_jpeg[1] == 0xD8);             /* SOI */
    DG_CHECK(s_jpeg[len - 2] == 0xFF && s_jpeg[len - 1] == 0xD9); /* EOI */
    int w = 0, h = 0;
    DG_CHECK(dg_jpeg_dimensions(s_jpeg, len, &w, &h) == DG_OK);
    DG_CHECK(w == EXP_W && h == EXP_H);        /* 等比降采样 320×160 → 160×80 */
}

/* 通道序回归(2026-10-14 紫屏事故):标准序纯红帧 → 编码 → 解码,
 * 中心像素必须 R 高 / G、B 低。首版把帧按 XRGB 字面序读(RGA 实际写
 * BGRA8888),红绿互换+蓝通道恒 255 = 整画面紫屏 */
static void test_channel_order(void)
{
    frame_push_color(0, 0, 255);               /* 纯红(标准字节序) */
    msleep(300);                               /* 等 worker 编码发布 */
    size_t len = 0;
    uint32_t seq = 0;
    DG_CHECK(grab(&len, &seq));
    static uint8_t rgb[SRC_W * SRC_H * 3];
    int w = 0, h = 0;
    DG_CHECK(dg_jpeg_decode_rgb(s_jpeg, len, 1, rgb, sizeof(rgb), &w, &h) ==
             DG_OK);
    int idx = ((h / 2) * w + w / 2) * 3;       /* 中心像素(避开边缘压缩伪影) */
    DG_CHECK(rgb[idx + 0] > 200);              /* R */
    DG_CHECK(rgb[idx + 1] < 60);               /* G */
    DG_CHECK(rgb[idx + 2] < 60);               /* B */
}

static void test_repeat_frame_skipped(uint32_t last)
{
    /* 不再推新帧:同帧不得重复编码发布 */
    msleep(400);
    size_t len = 0;
    uint32_t seq = 0;
    if (grab(&len, &seq))
        DG_CHECK(seq == last);
}

static void test_unsubscribe_stops(void)
{
    web_stream_viewer_remove();
    DG_CHECK(wait_for(claimed_off, 1000));     /* 退订后取帧开关被释放 */
    uint32_t before = 0;
    size_t len = 0;
    grab(&len, &before);
    for (int i = 0; i < 6; i++) {
        frame_push();
        msleep(66);
    }
    msleep(200);
    uint32_t after = 0;
    if (grab(&len, &after))
        DG_CHECK(after == before);             /* 退订后不再编码发布 */
    DG_CHECK(web_stream_active() == false);
}

static void test_disabled_rejected(void)
{
    setup_env(0);
    web_stream_stop();
    DG_CHECK(web_stream_start() == DG_OK);     /* 空转启动不算失败 */
    DG_CHECK(web_stream_available() == false);
    DG_CHECK(web_stream_viewer_add() == DG_ERR_UNSUPPORTED);
    web_stream_stop();
}

int main(void)
{
    setup_env(1);
    test_happy_path();

    size_t len = 0;
    uint32_t last = 0;
    test_channel_order();
    msleep(300);               /* 等 worker 排空最后一推,再取基线 seq(时序竞争) */
    grab(&len, &last);
    test_repeat_frame_skipped(last);
    test_unsubscribe_stops();
    test_disabled_rejected();

    web_stream_stop();
    storage_deinit();
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0)
        exit(1);
    DG_TEST_EXIT();
}
