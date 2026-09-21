/*
 * presenter_user_edit.c — 用户编辑页注册(视图内聚,动作经事件/直调 storage,
 * 与 page_users 同代风格)
 */
#include "presenter_user_edit.h"
#include "navigator/navigator.h"

extern void page_user_edit_create(lv_obj_t *parent);
extern void page_user_edit_destroy(void);
extern void page_user_edit_open(const char *uid);

void presenter_user_edit_register(void)
{
    static const navigator_page_t ops = {
        .name = "user_edit",
        .create = page_user_edit_create,
        .destroy = page_user_edit_destroy,
        .on_evt = page_user_edit_evt,
    };
    navigator_register(&ops);
}
