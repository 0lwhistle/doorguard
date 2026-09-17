/*
 * dg_list.h — 列表(记录查询/用户列表用)
 */
#ifndef DG_LIST_H
#define DG_LIST_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建列表容器(占满父容器,内部滚动) */
lv_obj_t *dg_list_create(lv_obj_t *parent);

/** 添加一行(图标 + 文本 + 点击回调;cb 可 NULL) */
lv_obj_t *dg_list_add_row(lv_obj_t *list, const char *icon, const char *text,
                          void (*on_click)(lv_event_t *e));

/** 清空列表(刷新查询结果前调用) */
void dg_list_clear(lv_obj_t *list);

#ifdef __cplusplus
}
#endif

#endif /* DG_LIST_H */
