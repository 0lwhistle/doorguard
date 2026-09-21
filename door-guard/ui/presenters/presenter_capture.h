/*
 * presenter_capture.h — 拍摄页接口
 */
#ifndef DG_PRESENTER_CAPTURE_H
#define DG_PRESENTER_CAPTURE_H

#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

void presenter_capture_register(void);

/** 打开拍摄页(navigator_push("capture") 前调用)。
 *  @param uid 目标用户(必须已存在:特征与头像都要挂在该用户上) */
void page_capture_open(const char *uid);

/** navigator on_evt 入口(质量/脸框/录入回执) */
void page_capture_evt(const ui_evt_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* DG_PRESENTER_CAPTURE_H */
