/**
 * @file lv_conf.h — door-guard 定制配置(LVGL v9.5.0)
 *
 * v9 的 lv_conf_internal.h 对未定义项有内置默认,这里只写覆盖项。
 * 迁移基线 = 8.3 的 lv_conf(third_party/lvgl/lv_conf.h),逐项对齐:
 * 颜色 32 / 256KB 内建内存池 / 刷新周期 30ms / montserrat 字号集合 /
 * 断言策略一致;差异项(NEON、OS)见下注。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR
 *====================*/
/* 板上 DRM dumb fb = XRGB8888(720x1280),与 8.3 相同 */
#define LV_COLOR_DEPTH 32

/*=========================
   MEMORY(对齐 8.3:内建分配器 + 256KB 固定池)
 *=========================*/
#define LV_USE_STDLIB_MALLOC      LV_STDLIB_BUILTIN
#define LV_MEM_SIZE               (256U * 1024U)   /* 8.3 同值 */
#define LV_MEM_POOL_EXPAND_SIZE   0                /* 固定池,等价 8.3 语义 */

/*====================
   HAL
 *====================*/
#define LV_DEF_REFR_PERIOD  30   /* 8.3 同值(LV_DISP_DEF_REFR_PERIOD 30) */
#define LV_DPI_DEF          130

/*=================
   OS:单线程主循环(lv_timer_handler 全权驱动),与 8.3 LV_TICK_CUSTOM=1 时代
   相同;时基用 lv_tick_set_cb 注入(display 后端/v9 DRM 驱动自带 CLOCK_MONOTONIC
   毫秒回调;C2 起 ui_init 在 display_init 之后重设为 dg_ui_tick_ms)
 *=================*/
#define LV_USE_OS   LV_OS_NONE

/*========================
   RENDERING
 *========================*/
/* v9.5 内置 NEON 加速为 C 内联实现(arm_neon.h),x86 宿主编不了;
 * 平台条件开:aarch64(板端 RK3576)= NEON,其余 = NONE。sim 与板共用本文件 */
#if defined(__aarch64__)
    #define LV_USE_DRAW_SW_ASM   LV_DRAW_SW_ASM_NEON
#else
    #define LV_USE_DRAW_SW_ASM   LV_DRAW_SW_ASM_NONE
#endif

#define LV_DRAW_SW_COMPLEX          1            /* 圆角/阴影/抗锯齿,8.3 同开 */
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#define LV_DRAW_SW_DRAW_UNIT_CNT     1           /* 单线程(LV_USE_OS=NONE 必须 1) */

#define LV_CACHE_DEF_SIZE             0          /* 8.3 LV_IMG_CACHE_DEF_SIZE 0 */
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_GRADIENT_MAX_STOPS         2

/*=============
   ASSERTS(8.3 同开两项快检)
 *=============*/
#define LV_USE_ASSERT_NULL    1
#define LV_USE_ASSERT_MALLOC  1

/*==================
   FONT USAGE
 *==================*/
/* 8.3 实开 14/28/48;契约 §2 追加 20。中文由 ui/font/dg_font_cn_16 提供
 * (v9 工具重生成,C2 完成;此前默认字体暂用 montserrat_14,C2 切回) */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

/* C2 字体重生成完成(v9 工具重跑 gen.sh),默认字体与 8.3 相同 */
#define LV_FONT_CUSTOM_DECLARE extern const lv_font_t dg_font_cn_16;
#define LV_FONT_DEFAULT &dg_font_cn_16

/*==================
   WIDGETS / THEMES / LAYOUTS
 *==================*/
/* 与 8.3 策略一致:widget 全量使能(差异:SIMPLE 主题为 v9 新增基础主题) */

/*==================
   DEVICES
 *==================*/
/* 板端显示:v9 内置 Linux DRM(dumb buffer x2,DIRECT 双缓冲 + atomic 翻转)。
 * 驱动源码用 xf86drm.h,交叉取 sysroot、宿主取 /usr/include/libdrm(CMake 注入) */
#define LV_USE_LINUX_DRM 1

/* 触摸:不复用 v9 evdev 驱动(无法提供 display.h 冻结契约要求的「按下沿」
 * 通知,也无 DG_TOUCH_* 板级校准钩),沿用自研 touch_evdev 喂 v9 indev
 * (契约 §3 允许的回退路径),故 LV_USE_EVDEV 关闭 */
#define LV_USE_EVDEV 0

/* sim 显示后端自写 SDL(v9 display API),不用 lv_sdl_window:需要
 * 按下沿通知与像素级行为可控(与板上 DRM 平面一致) */
#define LV_USE_SDL 0

/* 快照 API:不使能——整屏快照需 lv_malloc 3.6MB,超出 256KB 内建池;
 * 渲染取证走 sim 后端 SDL 读回(DG_SIM_DUMP_BMP)与板端 fb 导图 */
#define LV_USE_SNAPSHOT 0

/*=====================
   BUILD OPTIONS
 *======================*/
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS    0

#endif /*LV_CONF_H*/
