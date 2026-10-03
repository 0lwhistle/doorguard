/*
 * presenter_finger_set.c — 指纹管理页注册(视图内聚,同 face_set 代风格)
 */
#include "presenter_finger_set.h"
#include "navigator/navigator.h"

extern void page_finger_set_create(lv_obj_t *parent);
extern void page_finger_set_destroy(void);
extern void page_finger_set_evt(const ui_evt_t *evt);

void presenter_finger_set_register(void)
{
    static const navigator_page_t ops = {
        .name = "finger_set",
        .create = page_finger_set_create,
        .destroy = page_finger_set_destroy,
        .on_evt = page_finger_set_evt,
    };
    navigator_register(&ops);
}
