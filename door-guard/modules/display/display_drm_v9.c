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

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>   /* 走查取证:触发文件检查 */
#include <unistd.h>     /* unlink */

int display_init(void)
{
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        DG_LOGE("[DISPLAY]", "lv_linux_drm_create 失败");
        return DG_ERR_IO;
    }

    /* DG_UI_PLANE=1(video plane 直通阶段 A,2026-09-25 方案):display 换
     * ARGB8888——v9 渲染核心对带 alpha 的 display 逐脏区清透明(lv_refr.c
     * 内建,8.3 残影根因已除),screen 透明后不透明页自备底、透明页(主页)
     * 洞出下层;驱动 fourcc 跟随本设置(lv_linux_drm_set_file 内)。关=主线
     * XRGB 现状零影响 */
    bool ui_plane = false;
    const char *up = getenv("DG_UI_PLANE");
    if (up && up[0] == '1') {
        ui_plane = true;
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    }

    char *path = lv_linux_drm_find_device_path();
    lv_result_t res = lv_linux_drm_set_file(disp, path ? path : "/dev/dri/card0", -1);
    /* 路径由驱动 lv_zalloc(tlsf 池)分配:必须用 lv_free——libc free 会
     * abort("free(): invalid pointer",板上实测 C3)。宿主 sim 不编本文件,
     * 故 C1 未暴露 */
    lv_free(path);
    if (res != LV_RESULT_OK) {
        DG_LOGE("[DISPLAY]", "DRM 设备打开/配置失败");
        return DG_ERR_IO;
    }

    if (ui_plane) {
        /* screen 透明须在 set_file 成功后:失败路径不留下半透明状态。
         * bottom_layer 的透明由 set_color_format 内建处理,无需另设 */
        lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
    }

    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    if (w != DG_SCREEN_W || h != DG_SCREEN_H)
        DG_LOGW("[DISPLAY]", "DRM 分辨率 %dx%d 与设计 %dx%d 不同",
                w, h, DG_SCREEN_W, DG_SCREEN_H);

    /* 触摸注册失败不阻塞显示:web 上位机路径仍完整可用 */
    if (touch_evdev_start(w, h) != DG_OK)
        DG_LOGW("[DISPLAY]", "触摸未注册,UI 需经上位机操作");

    DG_LOGI("[DISPLAY]", "v9 DRM 后端就绪 %dx%d %s(direct 双缓冲 atomic 翻转%s)",
            w, h, ui_plane ? "ARGB8888" : "XRGB8888",
            ui_plane ? ",screen 透明" : "");
    return DG_OK;
}

void display_poll(void)
{
    /* ---- 走查取证(C3,诊断专用;v8 时代 DG_DUMP_FIRST_FRAME 的对等物) ----
     * 仅当 DG_WALK_DUMP_DIR 设置时生效:每帧检查触发文件 /tmp/dg_shot,
     * 存在则把「当前屏幕」(DIRECT 双缓冲经 sync_areas 收敛后两缓冲同像,
     * 取活动缓冲即整帧 XRGB8888,行距=dumb pitch)落 RAW 后删触发文件。
     * 生产路径(DG_WALK_DUMP_DIR 未设)只有一次 getenv,零开销 */
    static int walk_mode = -1;
    if (walk_mode < 0)
        walk_mode = getenv("DG_WALK_DUMP_DIR") != NULL;
    if (!walk_mode || stat("/tmp/dg_shot", &(struct stat){0}) != 0)
        return;
    unlink("/tmp/dg_shot");
    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_display_get_default());
    const char *dir = getenv("DG_WALK_DUMP_DIR");
    char path[128];
    snprintf(path, sizeof(path), "%s/dg_screen.raw", dir ? dir : "/tmp");
    FILE *f = fopen(path, "wb");
    if (f && buf) {
        fwrite(buf->data, 1, (size_t)buf->header.stride * buf->header.h, f);
        fclose(f);
        DG_LOGI("[DISPLAY]", "屏幕已导出 %s", path);
    } else if (f) {
        fclose(f);
    }
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
