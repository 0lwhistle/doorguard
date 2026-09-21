/*
 * presenter_capture.c — 拍摄页注册(视图内聚,与 user_edit 同代风格)
 */
#include "presenter_capture.h"
#include "navigator/navigator.h"

extern void page_capture_create(lv_obj_t *parent);
extern void page_capture_destroy(void);
extern void page_capture_evt(const ui_evt_t *evt);

void presenter_capture_register(void)
{
    static const navigator_page_t ops = {
        .name = "capture",
        .create = page_capture_create,
        .destroy = page_capture_destroy,
        .on_evt = page_capture_evt,
    };
    navigator_register(&ops);
}
