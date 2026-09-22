/*
 * lvgl9_smoke.c — LVGL 9.5 最小渲染验证入口(C1 门禁工具,不进产品)
 *
 * 目的:不依赖 ui/ 层,验证「v9 渲染栈 + display 后端」全链路:
 *   lv_init → tick → display_init(后端注册) → theme → 测试画面 → 主循环。
 * sim(SDL)与交叉编译产物(DRM)共用同一 main,验证画面覆盖:纯色底、
 * 圆角卡片、多字号文本(内置 montserrat)、按钮/开关/滑条、持续动画的
 * spinner(驱动脏区重绘,防「只画首帧」假阳性)。
 *
 * 自动取证(无图形环境/CI 可用):
 *   DG_SMOKE_FRAMES=<n>  跑 n 轮主循环后正常退出(缺省常驻,人工目检)
 *   sim 后端另支持 DG_SIM_DUMP_BMP=<路径>:首个刷新周期完成即把窗口像素
 *   存 BMP(见 display_sim_v9.c——验证真实 flush 链路;不用 lv_snapshot,
 *   整屏 lv_malloc 超出 256KB 内建池)
 * 板端配合 fb 导图/串口日志走查(C3 专项)。
 */
#include "dg_log.h"
#include "lvgl.h"
#include "modules/display/display.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint32_t smoke_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void build_test_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1b2a41), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 标题(48 号)+ 副标题(14 号):字号集合抽查 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL 9.5 SMOKE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "door-guard display backend test");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9fb3c8), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 130);

    /* 圆角卡片 + 20 号文本 */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 400, 220);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);

    lv_obj_t *card_label = lv_label_create(card);
    lv_label_set_text(card_label, "rounded card\nmulti-line");
    lv_obj_set_style_text_font(card_label, &lv_font_montserrat_20, 0);
    lv_obj_center(card_label);

    /* 按钮 / 开关 / 滑条 */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 180, 70);
    lv_obj_align(btn, LV_ALIGN_CENTER, -140, 120);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, LV_SYMBOL_OK " OK");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(btn_label);

    lv_obj_t *sw = lv_switch_create(scr);
    lv_obj_align(sw, LV_ALIGN_CENTER, 60, 120);

    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_width(slider, 300);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 220);
    lv_slider_set_value(slider, 60, LV_ANIM_OFF);

    /* 持续动画:spinner 让脏区重绘贯穿整个运行期 */
    lv_obj_t *spin = lv_spinner_create(scr);
    lv_obj_set_size(spin, 120, 120);
    lv_obj_align(spin, LV_ALIGN_BOTTOM_MID, 0, -80);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(smoke_tick_ms);

    if (display_init() != DG_OK) {
        DG_LOGE("[SMOKE]", "display_init 失败");
        return 1;
    }

    lv_display_t *disp = lv_display_get_default();
    lv_display_set_theme(disp, lv_theme_default_init(
                                   disp, lv_palette_main(LV_PALETTE_BLUE),
                                   lv_palette_main(LV_PALETTE_BLUE_GREY), false,
                                   &lv_font_montserrat_14));
    build_test_screen();

    const long frames = getenv("DG_SMOKE_FRAMES") ? atol(getenv("DG_SMOKE_FRAMES"))
                                                  : 0; /* 0 = 常驻 */
    for (long i = 0; frames == 0 || i < frames; i++) {
        uint32_t next = lv_timer_handler();
        display_poll();
        if (i > 0 && i % 300 == 0)
            DG_LOGI("[SMOKE]", "run %ld ms=%u", i, smoke_tick_ms());
        usleep((useconds_t)(next > 20 ? 20 : next) * 1000);
    }

    DG_LOGI("[SMOKE]", "smoke 完成 frames=%ld", frames);
    return 0;
}
