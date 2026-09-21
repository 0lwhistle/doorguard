/*
 * display.h — 显示 HAL(LVGL 显示驱动 + 触摸输入注册)
 *
 * PC 模拟器:SDL2 窗口 720×1280(DG_SIM 构建);
 * 板上:DRM/LVGL(Phase 8 接入,当前为占位)。
 * 同一份 ui/ 代码跑两处(spec-ui),业务与 UI 层不感知后端差异。
 */
#ifndef DG_DISPLAY_H
#define DG_DISPLAY_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化显示后端并注册 LVGL 驱动;须在 lv_init() 之后调用 */
int display_init(void);

/** 事件泵(SIM:SDL 事件→LVGL 输入;板:空实现)——主循环每帧调用 */
void display_poll(void);

/** 触摸活动监听:每次「按下沿」(手指落下瞬间)回调一次,UI 用来把
 *  「有操作」喂给服务层(EV_UI_TOUCH:清待机/菜单无操作计数)。
 *  注意是按下沿而非每次采样,否则 LVGL 30Hz 轮询会刷爆事件总线 */
void display_set_touch_listener(void (*fn)(void));

/** 后端内部用:按下沿通知(touch_evdev/display_sim 的 read_cb 调用) */
void display_touch_activity(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_DISPLAY_H */
