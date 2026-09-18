/*
 * page_home.h — 主页视图接口(pages 层;业务事件渲染在 presenter_home)
 */
#ifndef DG_PAGE_HOME_H
#define DG_PAGE_HOME_H

#include "lvgl.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

void page_home_create(lv_obj_t *parent);
void page_home_destroy(void);

/** 脸框渲染(state:DG_BOX_*;坐标为屏像素) */
void page_home_set_facebox(int state, int32_t x, int32_t y, int32_t w, int32_t h);
/** 只改脸框颜色(服务层命中/失败重新着色;位置沿用视觉后端逐帧刷新的检测框) */
void page_home_set_facebox_color(int state);
void page_home_clear_facebox(void);

/** 提示条(text 已翻译;NULL=隐藏) */
void page_home_set_hint(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_HOME_H */
