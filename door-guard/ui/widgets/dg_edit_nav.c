/*
 * dg_edit_nav.c — 编辑页统一导航实现
 *
 * 弹窗是全局单例(spec-auth-business:任意时刻一个弹窗),「保存退出/直接退出」
 * 的 choice 弹到哪一页都成立;pick 里保存成功才 back,失败留在页面继续编辑。
 */
#include "dg_edit_nav.h"
#include "dg_log.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "dg_btn.h"
#include "dg_popup.h"

#include <stdbool.h>

static const dg_edit_nav_ops_t *s_ops = NULL;

static void exit_now(void)
{
    navigator_back();
}

static void on_exit_pick(void *ud, int idx)
{
    (void)ud;
    if (idx == 0) {
        /* 保存退出:保存成功由 save 内部返回后退出;失败留在本页 */
        if (s_ops && s_ops->save && s_ops->save())
            exit_now();
        return;
    }
    if (s_ops && s_ops->discard)
        s_ops->discard();                    /* 直接退出:先丢页面级草稿 */
    exit_now();                              /* 直接退出(放弃修改) */
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (!s_ops || !s_ops->is_dirty || !s_ops->is_dirty()) {
        exit_now();
        return;
    }
    const char *const choices[] = { _("保存退出"), _("直接退出") };
    dg_popup_choice(_("有未保存的修改"), choices, 2, on_exit_pick, NULL, NULL);
}

static void on_save_btn(lv_event_t *e)
{
    (void)e;
    if (s_ops && s_ops->save)
        s_ops->save();
}

void dg_edit_nav_create(lv_obj_t *parent, const dg_edit_nav_ops_t *ops,
                        bool with_save)
{
    s_ops = ops;

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    if (with_save) {
        lv_obj_t *save = dg_btn_create(parent, LV_SYMBOL_OK, _("保存"));
        lv_obj_set_size(save, 150, 64);
        lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -DG_PAD, 16);
        lv_obj_add_event_cb(save, on_save_btn, LV_EVENT_CLICKED, NULL);
    }
}
