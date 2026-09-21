/*
 * presenter_user_edit.h — 用户编辑页(添加/编辑同一模板)
 */
#ifndef DG_PRESENTER_USER_EDIT_H
#define DG_PRESENTER_USER_EDIT_H

#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

void presenter_user_edit_register(void);

/** 打开编辑页(navigator_push("user_edit") 前调用)。
 *  @param uid 已存在用户;NULL/空 = 添加模式(ID 由列表页输入后经本函数带入) */
void page_user_edit_open(const char *uid);

/** navigator on_evt 入口(录入结果回执) */
void page_user_edit_evt(const ui_evt_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* DG_PRESENTER_USER_EDIT_H */
