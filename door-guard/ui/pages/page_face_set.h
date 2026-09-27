/*
 * page_face_set.h — 人脸识别设置页(检测/识别/活体三阈值,滑条即改即存)
 */
#ifndef PAGE_FACE_SET_H
#define PAGE_FACE_SET_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void page_face_set_create(lv_obj_t *parent);
void page_face_set_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* PAGE_FACE_SET_H */
