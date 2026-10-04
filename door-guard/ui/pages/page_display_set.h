/*
 * page_display_set.h — 屏幕显示设置页(背光亮度滑条,拖动即时生效)
 */
#ifndef PAGE_DISPLAY_SET_H
#define PAGE_DISPLAY_SET_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void page_display_set_create(lv_obj_t *parent);
void page_display_set_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* PAGE_DISPLAY_SET_H */
