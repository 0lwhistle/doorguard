/*
 * page_about.h — 关于设备页(presenter 回调入口)
 */
#ifndef DG_PAGE_ABOUT_H
#define DG_PAGE_ABOUT_H

#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

void page_about_create(lv_obj_t *parent);
void page_about_destroy(void);

/** 升级状态一拍(UI 事件泵 → LVGL 线程) */
void page_about_on_result(const ev_ota_update_t *ev);

/** broker 连接状态(检查更新按钮可用性) */
void page_about_on_mqtt(bool connected);

#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_ABOUT_H */
