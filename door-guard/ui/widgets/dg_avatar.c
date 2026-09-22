/*
 * dg_avatar.c — 头像显示控件实现
 *
 * 链路:db_user_get_avatar(自动解密)→ dg_jpeg_decode_rgb(缩放解码)→
 * RGB888 转 lv_color_t(XRGB8888,与 LV_COLOR_DEPTH=32 一致)→ lv_image_dsc_t。
 *
 * 缓存策略:按 size 分池(FULL 2 槽 / THUMB 8 槽),uid 命中即返,
 * 不命中轮转覆盖——列表页 6 行 + 编辑页 1 张,池容量就是按这个页面结构定的,
 * 不会每帧重解码(实际只在页面创建/刷新时 get 一次)。
 * 全部工作在 LVGL 线程(pages 刷新路径),无锁;缓存失效走显式 invalidate。
 */
#include "dg_avatar.h"
#include "dg_jpeg.h"
#include "storage.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FULL_SLOTS  2
#define THUMB_SLOTS 8
#define FULL_MAX    160                 /* 头像约定边长(proto 侧生成方保证 */
#define THUMB_MAX   (FULL_MAX / 4)      /* 40:libjpeg 1/4 缩放解码正好 */

typedef struct {
    char uid[DG_UID_LEN];
    lv_image_dsc_t dsc;
    uint8_t *px;                        /* malloc 的像素缓冲,dsc.data 指向它 */
    bool used;
} slot_t;

static slot_t s_full[FULL_SLOTS];
static slot_t s_thumb[THUMB_SLOTS];
static int s_full_pos, s_thumb_pos;

/* 解码暂存(仅 LVGL 线程使用):JPEG 明文 + RGB888 中转,避免逐槽扩栈 */
static uint8_t s_jpeg[DG_AVATAR_JPEG_MAX];
static uint8_t s_rgb[FULL_MAX * FULL_MAX * 3];

static slot_t *pool_pick(dg_avatar_size_t size)
{
    if (size == DG_AVATAR_FULL) {
        slot_t *s = &s_full[s_full_pos];
        s_full_pos = (s_full_pos + 1) % FULL_SLOTS;
        return s;
    }
    slot_t *s = &s_thumb[s_thumb_pos];
    s_thumb_pos = (s_thumb_pos + 1) % THUMB_SLOTS;
    return s;
}

/* RGB888 → lv_color_t:逐像素组色,LVGL 自身的位域排布保证端序正确 */
static void rgb_to_lv(const uint8_t *rgb, int n, lv_color_t *out)
{
    for (int i = 0; i < n; i++) {
        out[i] = lv_color_make(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }
}

static const lv_image_dsc_t *load_into(slot_t *s, const char *uid)
{
    size_t jlen = 0;
    const int denom = (s->dsc.header.w == THUMB_MAX) ? 4 : 1;
    int w = 0, h = 0;
    const bool ok =
        db_user_get_avatar(uid, s_jpeg, sizeof(s_jpeg), &jlen) == DG_OK &&
        jlen > 0 &&
        dg_jpeg_decode_rgb(s_jpeg, jlen, denom, s_rgb, sizeof(s_rgb), &w, &h)
            == DG_OK && w > 0 && h > 0;

    if (!ok) {
        /* 无头像/解码失败:槽位清干净,别让下一个同 uid 的查询脏命中旧图 */
        free(s->px);
        s->px = NULL;
        s->used = false;
        s->uid[0] = '\0';
        memset(&s->dsc, 0, sizeof(s->dsc));
        return NULL;
    }

    free(s->px);                        /* 轮转覆盖:旧像素让位 */
    s->px = malloc((size_t)w * h * sizeof(lv_color_t));
    if (!s->px) {
        s->used = false;
        s->uid[0] = '\0';
        memset(&s->dsc, 0, sizeof(s->dsc));
        return NULL;
    }
    rgb_to_lv(s_rgb, w * h, (lv_color_t *)s->px);

    memset(&s->dsc, 0, sizeof(s->dsc));
    s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;  /* v9:set_src 靠 magic 识别内存位图 */
    s->dsc.header.w = (uint32_t)w;
    s->dsc.header.h = (uint32_t)h;
    s->dsc.header.stride = (uint32_t)w * sizeof(lv_color_t);
    s->dsc.data_size = (uint32_t)((size_t)w * h * sizeof(lv_color_t));
    s->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    s->dsc.data = s->px;
    snprintf(s->uid, sizeof(s->uid), "%s", uid);
    s->used = true;
    return &s->dsc;
}

const lv_image_dsc_t *dg_avatar_get(const char *uid, dg_avatar_size_t size)
{
    if (!uid || !uid[0])
        return NULL;

    slot_t *pool = (size == DG_AVATAR_FULL) ? s_full : s_thumb;
    const int n = (size == DG_AVATAR_FULL) ? FULL_SLOTS : THUMB_SLOTS;
    for (int i = 0; i < n; i++) {
        if (pool[i].used && strcmp(pool[i].uid, uid) == 0)
            return &pool[i].dsc;        /* 命中:近期查询过的 uid 不重解码 */
    }
    slot_t *s = pool_pick(size);
    /* 目标尺寸经 dsc.header.w 传给 load_into 选缩放倍率:先填假头再装载 */
    s->dsc.header.w = (size == DG_AVATAR_FULL) ? FULL_MAX : THUMB_MAX;
    return load_into(s, uid);
}

void dg_avatar_invalidate(const char *uid)
{
    const size_t len = (uid && uid[0]) ? strlen(uid) : 0;
    for (int p = 0; p < 2; p++) {
        slot_t *pool = p ? s_thumb : s_full;
        const int n = p ? THUMB_SLOTS : FULL_SLOTS;
        for (int i = 0; i < n; i++) {
            if (!pool[i].used)
                continue;
            if (len == 0 || strncmp(pool[i].uid, uid, DG_UID_LEN) == 0) {
                free(pool[i].px);
                pool[i].px = NULL;
                pool[i].used = false;
                pool[i].uid[0] = '\0';
                memset(&pool[i].dsc, 0, sizeof(pool[i].dsc));
            }
        }
    }
}
