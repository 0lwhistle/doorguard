/*
 * display_drm_v9.c — 板上显示后端(LVGL 9.5 内置 Linux DRM 驱动)
 *
 * 为什么用 v9 内置驱动而非自研 flush:v9 的 linux_drm 后端(dumb buffer x2,
 * DIRECT 双缓冲 + atomic 翻转 + vblank 等待)与 8.3 时代已验证的
 * 「full_refresh 双缓冲页翻转」架构等价——脏区渲染 + sync_areas 双向同步 +
 * 垂直同步翻转,无残影语义由 v9 内建(refr_sync_areas),不必重走
 * 2026-09-22 video plane 的擦除语义深坑(DEVLOG 深夜条)。
 * 若板上 RK3576 实测有坑(atomic/分辨率/刷新异常),预案=复用已验证的
 * legacy modeset 自研 flush(契约 §2 D2),本文件只换 flush 层。
 *
 * 触摸:自研 touch_evdev(Type-B MT + 按下沿 + DG_TOUCH_* 校准)喂 v9 indev。
 * 注意:v9 DRM 驱动在 lv_linux_drm_create() 内部会 lv_tick_set_cb(自带
 * CLOCK_MONOTONIC 毫秒回调);C2 的 ui_init 在 display_init 之后执行,
 * 届时 lv_tick_set_cb(dg_ui_tick_ms) 覆盖为 ui 时基,顺序天然正确。
 */
#include "display.h"
#include "dg_log.h"
#include "lvgl.h"       /* LV_USE_LINUX_DRM=1 时 lvgl.h 已暴露 lv_linux_drm.h */
#include "touch_evdev.h"
#include "ui/theme.h"

#include <stdlib.h>     /* free(lv_linux_drm_find_device_path 返回值) */

int display_init(void)
{
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        DG_LOGE("[DISPLAY]", "lv_linux_drm_create 失败");
        return DG_ERR_IO;
    }

    char *path = lv_linux_drm_find_device_path();
    lv_result_t res = lv_linux_drm_set_file(disp, path ? path : "/dev/dri/card0", -1);
    free(path);
    if (res != LV_RESULT_OK) {
        DG_LOGE("[DISPLAY]", "DRM 设备打开/配置失败");
        return DG_ERR_IO;
    }

    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    if (w != DG_SCREEN_W || h != DG_SCREEN_H)
        DG_LOGW("[DISPLAY]", "DRM 分辨率 %dx%d 与设计 %dx%d 不同",
                w, h, DG_SCREEN_W, DG_SCREEN_H);

    /* 触摸注册失败不阻塞显示:web 上位机路径仍完整可用 */
    if (touch_evdev_start(w, h) != DG_OK)
        DG_LOGW("[DISPLAY]", "触摸未注册,UI 需经上位机操作");

    DG_LOGI("[DISPLAY]", "v9 DRM 后端就绪 %dx%d XRGB8888(direct 双缓冲 atomic 翻转)",
            w, h);
    return DG_OK;
}

void display_poll(void)
{
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
