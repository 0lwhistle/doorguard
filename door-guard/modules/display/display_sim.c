/*
 * display_sim.c — PC 模拟器显示后端(SDL2 窗口 720×1280)
 *
 * 为什么自写而不用 lv_drivers:官方 lv_drivers 已停维且引 FreeRTOS 遗留
 * 依赖;LVGL 8.3 显示驱动接口很小(缓冲 + flush + 输入),~150 行自实现
 * 换来零第三方纠缠。模拟器仅 UI 调试用,像素行为与板上 DRM 平面一致
 * (同 ARGB8888 直通)。
 */
#include "display.h"
#include "SDL2/SDL.h"
#include "dg_log.h"
#include "lvgl.h"
#include "ui/theme.h"

#include <stdlib.h>

static const char *TAG = "[DISPLAY]";

static SDL_Window *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture *s_tex = NULL;
static lv_color_t *s_lvbuf = NULL;       /* LVGL 渲染缓冲(整帧) */
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    SDL_Rect rect = { area->x1, area->y1, w, h };

    /* 行序拷进纹理(整帧缓冲时可直接整帧更新,这里按区域保守更新) */
    SDL_UpdateTexture(s_tex, &rect, color_p, w * sizeof(lv_color_t));
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    lv_disp_flush_ready(drv);
    (void)color_p;
}

/* SDL 鼠标 → LVGL 指针(模拟触摸) */
static void read_pointer(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int mx = 0, my = 0;
    uint32_t buttons = SDL_GetMouseState(&mx, &my);
    data->point.x = (lv_coord_t)mx;
    data->point.y = (lv_coord_t)my;
    const lv_indev_state_t st = (buttons & SDL_BUTTON_LMASK)
                                    ? LV_INDEV_STATE_PRESSED
                                    : LV_INDEV_STATE_RELEASED;
    /* 按下沿才通知:LVGL 每拍把 data 清零,上一拍状态要自己记 */
    static bool was_pressed;
    if (st == LV_INDEV_STATE_PRESSED && !was_pressed)
        display_touch_activity();
    was_pressed = (st == LV_INDEV_STATE_PRESSED);
    data->state = st;
    (void)drv;
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
    if (!s_ren)
        goto fail;
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DG_SCREEN_W, DG_SCREEN_H);
    if (!s_tex)
        goto fail;

    s_lvbuf = malloc((size_t)DG_SCREEN_W * DG_SCREEN_H * sizeof(lv_color_t));
    if (!s_lvbuf) {
        DG_LOGE(TAG, "framebuffer alloc failed");
        goto fail;
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_lvbuf, NULL, DG_SCREEN_W * DG_SCREEN_H);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = DG_SCREEN_W;
    s_disp_drv.ver_res = DG_SCREEN_H;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = flush_cb;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = read_pointer;
    lv_indev_drv_register(&s_indev_drv);

    DG_LOGI(TAG, "SDL 后端就绪 %dx%d", DG_SCREEN_W, DG_SCREEN_H);
    return DG_OK;

fail:
    DG_LOGE(TAG, "SDL 资源创建失败: %s", SDL_GetError());
    if (s_tex)
        SDL_DestroyTexture(s_tex);
    if (s_ren)
        SDL_DestroyRenderer(s_ren);
    if (s_win)
        SDL_DestroyWindow(s_win);
    free(s_lvbuf);
    s_lvbuf = NULL;
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
