/*
 * camera_sim.c — PC 模拟器相机后端:目录图片循环播放
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "stb_image.h"

#include "camera.h"
#include "dg_log.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[CAMERA]";

#define SIM_IMG_MAX 8

typedef struct {
    uint8_t *pixels;           /* RGB888 */
    int32_t w, h;
} sim_image_t;

static sim_image_t s_imgs[SIM_IMG_MAX];
static int s_img_cnt = 0;
static int s_cur = 0;
static uint32_t s_seq = 0;
static uint32_t s_interval_ms = 66;   /* ~15fps */
static int64_t s_next_ms = 0;
static camera_frame_fn s_cb = NULL;
static void *s_ud = NULL;

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int camera_init(const char *res_path, camera_frame_fn cb, void *ud)
{
    if (!res_path || !cb)
        return DG_ERR_PARAM;

    DIR *d = opendir(res_path);
    if (!d) {
        DG_LOGW(TAG, "sim 图片目录 %s 打不开,相机未就绪", res_path);
        return DG_ERR_IO;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_img_cnt < SIM_IMG_MAX) {
        size_t len = strlen(ent->d_name);
        if (len < 4)
            continue;
        const char *ext = ent->d_name + len - 4;
        if (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") &&
            strcasecmp(ext, ".bmp"))
            continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", res_path, ent->d_name);
        int w, h, comp;
        uint8_t *img = stbi_load(full, &w, &h, &comp, 3);   /* 强制 RGB */
        if (!img) {
            DG_LOGW(TAG, "解码失败 %s", full);
            continue;
        }
        s_imgs[s_img_cnt].pixels = img;
        s_imgs[s_img_cnt].w = w;
        s_imgs[s_img_cnt].h = h;
        s_img_cnt++;
        DG_LOGI(TAG, "sim 图片 %s %dx%d", ent->d_name, w, h);
    }
    closedir(d);

    if (s_img_cnt == 0) {
        DG_LOGW(TAG, "sim 目录无可用图片,相机未就绪");
        return DG_ERR_NOT_FOUND;
    }

    s_cb = cb;
    s_ud = ud;
    s_next_ms = mono_ms();
    DG_LOGI(TAG, "sim 相机就绪:%d 张,%ums/帧", s_img_cnt, s_interval_ms);
    return DG_OK;
}

void camera_sim_set_interval(uint32_t ms)
{
    s_interval_ms = ms;
}

void camera_poll(void)
{
    if (!s_cb || s_img_cnt == 0)
        return;
    int64_t now = mono_ms();
    if (now < s_next_ms)
        return;
    s_next_ms = now + s_interval_ms;

    sim_image_t *img = &s_imgs[s_cur];
    camera_frame_t frame = {
        .w = img->w,
        .h = img->h,
        .seq = ++s_seq,
        .pixels = img->pixels,
    };
    s_cb(&frame, s_ud);
    s_cur = (s_cur + 1) % s_img_cnt;
}
