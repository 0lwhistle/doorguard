/*
 * display_drm.c — 板上显示后端(lv_drivers DRM dumb-buffer,MIT)
 *
 * 为什么不用 /dev/fb0:B4 固件 rockchipdrmfb 的 mmap 返回 EBUSY(实测,
 * fbdev 仿真层限制);lv_demo 实际走 DRM。本后端经 libdrm 创建 dumb
 * buffer + legacy modeset,与 lv_drivers/display/drm.c 一致。
 * 触摸输入:evdev 自动探测(fts_ts/goodix,见 touch_evdev.c),注册 pointer indev。
 */
#include "display.h"
#include "dg_log.h"
#include "lvgl.h"
#include "../../third_party/lv_drivers/drm.h"
#include "touch_evdev.h"
#include "ui/theme.h"

#include <stdlib.h>
#include <stdio.h>

void drm_dump_first_frame_once(const lv_color_t *frame, int32_t w, int32_t h);
static void drm_flush_wrapper(lv_disp_drv_t *drv, const lv_area_t *area,
                              lv_color_t *color_p);

int display_init(void)
{
    drm_init();
    lv_coord_t w = 0, h = 0;
    drm_get_sizes(&w, &h, NULL);
    if (w != DG_SCREEN_W || h != DG_SCREEN_H)
        DG_LOGW("[DISPLAY]", "DRM 分辨率 %dx%d 与设计 %dx%d 不同",
                w, h, DG_SCREEN_W, DG_SCREEN_H);

    /* DRM 驱动自带 draw buffer 与 flush(drm_flush 内部双缓冲页翻转) */
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t *buf1;
    static lv_color_t *buf2;
    buf1 = malloc((size_t)w * h * sizeof(lv_color_t));
    buf2 = malloc((size_t)w * h * sizeof(lv_color_t));
    if (!buf1 || !buf2) {
        DG_LOGE("[DISPLAY]", "DRM 渲染缓冲分配失败");
        return DG_ERR_NO_MEMORY;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, w * h);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = w;
    disp_drv.ver_res = h;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = drm_flush_wrapper;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

    /* 触摸注册失败不阻塞显示:web 上位机路径仍完整可用 */
    if (touch_evdev_start(w, h) != DG_OK)
        DG_LOGW("[DISPLAY]", "触摸未注册,UI 需经上位机操作");

    DG_LOGI("[DISPLAY]", "DRM 后端就绪 %dx%d flush_cb=%p", w, h,
            (void *)disp_drv.flush_cb);
    return DG_OK;
}

static void drm_flush_wrapper(lv_disp_drv_t *drv, const lv_area_t *area,
                              lv_color_t *color_p)
{
    static int n = 0;
    if (n < 3)
        DG_LOGI("[DISPLAY]", "flush #%d area=%d,%d %dx%d", n, area->x1, area->y1,
                area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
    n++;
    drm_flush(drv, area, color_p);
    if (n == 20)                          /* 第 20 帧:页面已完整渲染 */
        drm_dump_first_frame_once(color_p, drv->hor_res, drv->ver_res);
    /* 注意:drm_dump_first_frame_once 内部受 DG_DUMP_FIRST_FRAME 环境变量
     * 门控,未设置时立即返回,生产路径无 IO */
}

void display_poll(void)
{
}

/* 首帧导出(板上取证:DG_DUMP_FIRST_FRAME=1 时写 /tmp/dg_frame.raw,
 * ARGB8888 720x1280;仅首帧一次,不影响运行) */
static int s_dump_done = 0;

void drm_dump_first_frame_once(const lv_color_t *frame, int32_t w, int32_t h)
{
    if (s_dump_done || getenv("DG_DUMP_FIRST_FRAME") == NULL)
        return;
    FILE *f = fopen("/tmp/dg_frame.raw", "wb");
    if (f) {
        fwrite(frame, sizeof(lv_color_t), (size_t)w * h, f);
        fclose(f);
        DG_LOGI("[DISPLAY]", "首帧已导出 /tmp/dg_frame.raw");
    }
    s_dump_done = 1;
}

/* ---- video plane 直通(2026-09-22 实验,已回退) ----
 * direct+ARGB underlay 在 LVGL8.3 上存在透明页"擦除语义缺失"(切页残影,
 * 详见 DEVLOG 2026-09-22 深夜条目与 git f519b1e 的完整实现)。当前恒回退
 * 软渲染路径;待 LVGL9 迁移后以 f519b1e 为基础重启本特性 */
bool display_has_video_plane(void)
{
    return false;
}

int display_video_plane_show(int slot, int dmabuf_fd, int32_t w, int32_t h,
                             int32_t stride)
{
    (void)slot; (void)dmabuf_fd; (void)w; (void)h; (void)stride;
    return DG_ERR_UNSUPPORTED;
}

void display_video_plane_hide(void)
{
}

void display_clear_fbs(void)
{
}
