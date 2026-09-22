/*
 * display_sim_v9.c — PC 模拟器显示后端(LVGL 9.5 + SDL2 窗口 720×1280)
 *
 * v8 版(display_sim.c)的同构移植:自写 SDL 后端而非 lv_sdl_window,
 * 因为 ①display.h 冻结契约要求「按下沿」通知(display_touch_activity),
 * v9 内置 SDL 鼠标驱动无此钩子;②像素行为与板上 DRM 平面保持一致。
 * v9 渲染模型:PARTIAL 模式,flush_cb 收到「脏区 + 紧凑像素」,逐区更新纹理。
 *
 * 取证:DG_SIM_DUMP_BMP=<路径> 时,首个刷新周期完成后把窗口像素存为 BMP
 * (dg_lvgl9_smoke 自动验证与人工走查共用),与 v8 板端 DG_DUMP_FIRST_FRAME
 * 同一套 env 门控思路,生产路径无 IO。
 */
#include "display.h"
#include "SDL2/SDL.h"
#include "dg_log.h"
#include "lvgl.h"
#include "ui/theme.h"

#include <stdlib.h>
#include "err.h"

static const char *TAG = "[DISPLAY]";

static SDL_Window *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture *s_tex = NULL;
static lv_display_t *s_disp = NULL;

static void dump_once_if_requested(void);

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    SDL_Rect rect = { area->x1, area->y1, w, h };

    /* v9 PARTIAL 模式:px_map 为该脏区的紧凑像素,行距 = 脏区宽 */
    SDL_UpdateTexture(s_tex, &rect, px_map, (int)w * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    lv_display_flush_ready(disp);

    dump_once_if_requested();
}

/* DG_SIM_DUMP_BMP=路径:导图目标。首个刷新周期(最后一块脏区)后导图一次。
 * DG_SIM_DUMP_AFTER=<n>:改在第 n 个刷新周期导(九页走查工具逐页指定);
 * 转储完成即自清 DG_SIM_DUMP_BMP,保证「一次一图」 */
static void dump_once_if_requested(void)
{
    if (!lv_display_flush_is_last(s_disp))
        return;
    const char *path = getenv("DG_SIM_DUMP_BMP");
    if (path == NULL || path[0] == '\0')
        return;
    static uint32_t cycle_cnt;
    const char *after = getenv("DG_SIM_DUMP_AFTER");
    if (after != NULL && after[0] != '\0') {
        long n = atol(after);
        if (n > 0 && ++cycle_cnt < (uint32_t)n)
            return;
        cycle_cnt = 0;
    }

    int w = 0, h = 0;
    if (SDL_QueryTexture(s_tex, NULL, NULL, &w, &h) != 0) {
        DG_LOGW(TAG, "导图跳过: %s", SDL_GetError());
        setenv("DG_SIM_DUMP_BMP", "", 1);
        return;
    }
    void *pixels = malloc((size_t)w * h * 4);
    if (!pixels) {
        DG_LOGW(TAG, "导图跳过: 内存不足");
        setenv("DG_SIM_DUMP_BMP", "", 1);
        return;
    }
    const int pitch = w * 4;
    if (SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888,
                             pixels, pitch) != 0) {
        DG_LOGW(TAG, "导图跳过(读回失败): %s", SDL_GetError());
        free(pixels);
        setenv("DG_SIM_DUMP_BMP", "", 1);
        return;
    }
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, w, h, 32, pitch, SDL_PIXELFORMAT_ARGB8888);
    int save = surf ? SDL_SaveBMP(surf, path) : -1;
    if (surf)
        SDL_FreeSurface(surf);
    free(pixels);
    if (save == 0)
        DG_LOGI(TAG, "画面已导出 %s", path);
    else
        DG_LOGW(TAG, "导图失败: %s", SDL_GetError());
    setenv("DG_SIM_DUMP_BMP", "", 1);      /* 一次性:转储后自清 */
}

/* SDL 鼠标 → LVGL 指针(模拟触摸) */
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    int mx = 0, my = 0;
    uint32_t buttons = SDL_GetMouseState(&mx, &my);
    data->point.x = (int32_t)mx;
    data->point.y = (int32_t)my;
    const lv_indev_state_t st = (buttons & SDL_BUTTON_LMASK)
                                    ? LV_INDEV_STATE_PRESSED
                                    : LV_INDEV_STATE_RELEASED;
    /* 按下沿才通知:LVGL 每拍把 data 清零,上一拍状态要自己记 */
    static bool was_pressed;
    if (st == LV_INDEV_STATE_PRESSED && !was_pressed)
        display_touch_activity();
    was_pressed = (st == LV_INDEV_STATE_PRESSED);
    data->state = st;
    (void)indev;
}

int display_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        DG_LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return DG_ERR_IO;
    }
    s_win = SDL_CreateWindow("door-guard sim", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, DG_SCREEN_W, DG_SCREEN_H, 0);
    if (!s_win)
        goto fail;
    s_ren = SDL_CreateRenderer(s_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) /* 无 GPU 环境(如 SDL_VIDEODRIVER=dummy 自动化)退软件渲染 */
        s_ren = SDL_CreateRenderer(s_win, -1, 0);
    if (!s_ren)
        goto fail;
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DG_SCREEN_W, DG_SCREEN_H);
    if (!s_tex)
        goto fail;

    s_disp = lv_display_create(DG_SCREEN_W, DG_SCREEN_H);
    if (!s_disp) {
        DG_LOGE(TAG, "lv_display_create failed");
        goto fail;
    }
    /* 单整帧缓冲 + PARTIAL:v9 在其中按脏区紧凑渲染,flush_cb 逐区上屏 */
    static lv_color_t *buf;
    buf = malloc((size_t)DG_SCREEN_W * DG_SCREEN_H * 4);
    if (!buf) {
        DG_LOGE(TAG, "framebuffer alloc failed");
        goto fail;
    }
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, buf, NULL,
                           (uint32_t)DG_SCREEN_W * DG_SCREEN_H * 4,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    if (!indev)
        goto fail;
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_pointer);
    lv_indev_set_display(indev, s_disp);

    DG_LOGI(TAG, "SDL 后端就绪 %dx%d (lvgl9)", DG_SCREEN_W, DG_SCREEN_H);
    return DG_OK;

fail:
    DG_LOGE(TAG, "SDL 资源创建失败: %s", SDL_GetError());
    if (s_tex)
        SDL_DestroyTexture(s_tex);
    if (s_ren)
        SDL_DestroyRenderer(s_ren);
    if (s_win)
        SDL_DestroyWindow(s_win);
    s_tex = NULL;
    s_ren = NULL;
    s_win = NULL;
    s_disp = NULL;
    SDL_Quit();
    return DG_ERR_IO;
}

void display_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            exit(0);                    /* 关窗即退出:模拟器是开发工具 */
    }
}

/* ---- video plane 直通:sim 无硬件 plane,恒不可用(页面走软渲染回退) ---- */
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
