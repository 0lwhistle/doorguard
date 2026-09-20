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

#ifdef __cplusplus
}
#endif

#endif /* DG_DISPLAY_H */
