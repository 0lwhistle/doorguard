/*
 * presenter_face_set.c — face_set 页展示器:仅注册(页面无异步事件)
 */
#include "presenter_face_set.h"
#include "navigator/navigator.h"

extern void page_face_set_create(lv_obj_t *parent);
extern void page_face_set_destroy(void);

void presenter_face_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "face_set",
        .create = page_face_set_create,
        .destroy = page_face_set_destroy,
    };
    navigator_register(&ops);
}
