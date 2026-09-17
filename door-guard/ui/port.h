/*
 * port.h — UI 平台端口(时基供 lv_conf LV_TICK_CUSTOM 引用)
 */
#ifndef DG_UI_PORT_H
#define DG_UI_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** LVGL 毫秒时基(CLOCK_MONOTONIC;lv_conf.h LV_TICK_CUSTOM 引用) */
uint32_t dg_ui_tick_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_UI_PORT_H */
