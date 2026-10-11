/*
 * page_device_data.h — 设备数据页(设备管理 → 关于设备 → 设备数据;
 * 四个破坏性操作的二次确认入口)
 */
#ifndef DG_PAGE_DEVICE_DATA_H
#define DG_PAGE_DEVICE_DATA_H

#include "events.h"
#include "lvgl.h"      /* lv_obj_t(本头自洽;页面 .c 不一定先 include lvgl) */

#ifdef __cplusplus
extern "C" {
#endif

void page_device_data_create(lv_obj_t *parent);
void page_device_data_destroy(void);

/** 设备数据维护回执(presenter 转发 UI_EVT_DATA_OP_RESULT;页面按 op 配对) */
void page_device_data_on_result(const ev_sys_data_op_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_DEVICE_DATA_H */
