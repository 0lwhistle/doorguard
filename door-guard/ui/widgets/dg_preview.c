/*
 * dg_preview.c — 相机预览控件(2026-09-22 video-plane 直通改造)
 *
 * 双模:plane(display_has_video_plane)→ 透明占位 + camera_latest_dmabuf
 * 直送 VOP2,预览像素零 CPU;否则 lv_img 软渲染(原幅直通,板测 26~27fps,
 * 详见 DEVLOG 2026-09-22)。两条路径共用同一套 seq 去重与节流日志。
 */
#include "dg_preview.h"
#include "dg_log.h"
#include "../../modules/camera/camera.h"
#include "../theme.h"
#include "modules/display/display.h"
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char tag[10];
    bool plane;                /* true=硬件 plane 直通;false=软渲染回退 */
    /* 软渲染路径状态 */
    lv_obj_t *img;             /* 回退路径的 lv_img 控件 */
    lv_image_dsc_t dsc;
    uint32_t *buf;
    int32_t bw, bh;
    int32_t fw, fh;
    /* 公共:去重 + 节流统计 */
    uint32_t last_seq;
    uint32_t frames, stat_t0;
} st_t;

static void stat_tick(st_t *st, uint32_t now, uint32_t cost_ms)
{
    st->frames++;
    if (!st->stat_t0)
        st->stat_t0 = now;
    if (now - st->stat_t0 >= 4000) {
        DG_LOGI(st->tag, "preview fps=%u cost=%ums",
                st->frames * 1000u / (now - st->stat_t0), cost_ms);
        st->frames = 0;
        st->stat_t0 = now;
    }
}

/* ---- plane 直通路径 ---- */

static void pump_plane(st_t *st)
{
    const camera_dmabuf_t *f = camera_latest_dmabuf();
    if (!f || f->seq == st->last_seq)
        return;
    uint32_t t0 = dg_ui_tick_ms();
    if (display_video_plane_show(f->slot, f->fd, f->w, f->h, f->stride) == DG_OK)
        camera_dmabuf_mark_shown(f->slot);
    st->last_seq = f->seq;
    stat_tick(st, dg_ui_tick_ms(), dg_ui_tick_ms() - t0);
}

/* ---- 软渲染回退路径(原 page_home 逻辑) ---- */

static void fallback_setup(st_t *st, const camera_frame_t *f)
{
    /* DG_UI_PREVIEW_DEC 标定档:1=原幅直通(默认,板测 26fps);2/4=降采样
     * +zoom(拷贝省但 LVGL8.3 变换路径慢,实测仅 19fps)。小帧原样进 */
    int dec = 0;
    const char *e = getenv("DG_UI_PREVIEW_DEC");
    if (e && (*e == '1' || *e == '2' || *e == '4'))
        dec = *e - '0';
    if (!dec)
        dec = 1;

    uint32_t *buf = malloc((size_t)f->w / dec * (f->h / dec) * 4);
    if (!buf)
        return;
    free(st->buf);
    st->buf = buf;
    st->bw = f->w / dec;
    st->bh = f->h / dec;
    st->fw = f->w;
    st->fh = f->h;

    memset(&st->dsc, 0, sizeof(st->dsc));
    st->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;  /* v9:set_src 靠 magic 识别内存位图 */
    st->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    st->dsc.header.w = (uint32_t)st->bw;
    st->dsc.header.h = (uint32_t)st->bh;
    st->dsc.header.stride = (uint32_t)st->bw * 4;
    st->dsc.data = (const uint8_t *)st->buf;
    st->dsc.data_size = (size_t)st->bw * st->bh * 4;

    if (!st->img)
        return;
    lv_image_set_src(st->img, &st->dsc);
    lv_image_set_antialias(st->img, false);
    lv_image_set_pivot(st->img, st->bw / 2, st->bh / 2);
    int32_t zx = ((int32_t)256 * DG_SCREEN_W + st->bw / 2) / st->bw;
    int32_t zy = ((int32_t)256 * DG_SCREEN_H + st->bh / 2) / st->bh;
    lv_image_set_scale(st->img, (uint16_t)(zx > zy ? zx : zy));
    lv_obj_center(st->img);
}

static void fallback_blit(st_t *st, const camera_frame_t *f)
{
    const uint32_t *src = (const uint32_t *)f->pixels;
    int32_t sx = f->w / st->bw, sy = f->h / st->bh;
    if (sx == 1 && sy == 1) {
        for (int32_t y = 0; y < st->bh; y++)
            memcpy(st->buf + (size_t)y * st->bw, src + (size_t)y * f->w,
                   (size_t)st->bw * 4);
        return;
    }
    for (int32_t y = 0; y < st->bh; y++) {
        const uint32_t *sr = src + (size_t)(y * sy) * f->w;
        uint32_t *dr = st->buf + (size_t)y * st->bw;
        for (int32_t x = 0; x < st->bw; x++)
            dr[x] = sr[x * sx];
    }
}

static void pump_fallback(st_t *st)
{
    const camera_frame_t *f = camera_latest();
    if (!f || f->seq == st->last_seq)
        return;
    if (!st->buf || f->w != st->fw || f->h != st->fh)
        fallback_setup(st, f);
    if (!st->buf)
        return;
    uint32_t t0 = dg_ui_tick_ms();
    fallback_blit(st, f);
    st->last_seq = f->seq;
    if (st->img)
        lv_obj_invalidate(st->img);
    stat_tick(st, dg_ui_tick_ms(), dg_ui_tick_ms() - t0);
}

/* ---- 生命周期 ---- */

lv_obj_t *dg_preview_create(lv_obj_t *parent, const char *log_tag)
{
    st_t *st = calloc(1, sizeof(st_t));
    if (!st)
        return NULL;
    snprintf(st->tag, sizeof(st->tag), "%s", log_tag);

    lv_obj_t *p;
    if (display_has_video_plane()) {
        st->plane = true;
        p = lv_obj_create(parent);           /* 透明占位:像素走硬件 plane */
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, DG_SCREEN_W, DG_SCREEN_H);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
        camera_rgb_preview_set(false);       /* XRGB 转换不再需要,省一次 RGA */
        DG_LOGI(st->tag, "preview=video-plane 直通");
    } else {
        p = lv_image_create(parent);
        lv_image_set_antialias(p, false);
        st->img = p;
        camera_rgb_preview_set(true);
        DG_LOGI(st->tag, "preview=软渲染回退");
    }
    lv_obj_set_user_data(p, st);
    return p;
}

void dg_preview_pump(lv_obj_t *p)
{
    st_t *st = p ? (st_t *)lv_obj_get_user_data(p) : NULL;
    if (!st)
        return;
    if (st->plane)
        pump_plane(st);
    else
        pump_fallback(st);
}

void dg_preview_destroy(lv_obj_t *p)
{
    st_t *st = p ? (st_t *)lv_obj_get_user_data(p) : NULL;
    if (!st)
        return;
    if (st->plane) {
        display_video_plane_hide();
        camera_dmabuf_mark_shown(-1);
    }
    free(st->buf);
    free(st);
    lv_obj_set_user_data(p, NULL);
}
