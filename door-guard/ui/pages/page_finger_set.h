/*
 * page_finger_set.h — 指纹管理页(2026-10-03 用户反馈:录入别再挤在编辑页弹窗里)
 *
 * 编辑页指纹行进入:指纹一/二/三三个槽位各自 显示状态 + 录入或删除;
 * 录入走页内专用引导窗(指纹图 + 请按指纹/请再按一次 + 5s 无按压自动退出),
 * 不再借用「验证失败」样式的弹窗做过程提示。
 */
#ifndef DG_PAGE_FINGER_SET_H
#define DG_PAGE_FINGER_SET_H

#include "lvgl.h"
#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设定编辑对象(uid 须已存在;ADD 未保存用户不可进本页,入口拦) */
void page_finger_set_open(const char *uid);

void page_finger_set_create(lv_obj_t *parent);
void page_finger_set_destroy(void);
void page_finger_set_evt(const ui_evt_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_FINGER_SET_H */
