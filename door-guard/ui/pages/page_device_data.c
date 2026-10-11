/*
 * page_device_data.c — 设备数据页(设备管理 → 关于设备 → 设备数据;2026-10-11)
 *
 * 四个破坏性操作(清指纹/清用户/清数据/复位配置),全部红底 + 二次确认。
 * 交互:点击 → dg_popup_choice_ex 确认 → on_pick 里 s_pending_op 先记后发
 * (bridge_data_op),回执经 UI_EVT_DATA_OP_RESULT 由 presenter 转回本页,
 * 按 s_pending_op 配对——陈旧或他页的回执一律丢弃。
 * 结果弹窗在本页发(与 about 页同款):收到回执即弹,不持有任何控件指针;
 * 页面销毁时把 s_pending_op 归 -1,迟到的回执自然不匹配、不会弹到别的页上。
 */
#include "page_device_data.h"
#include "dg_log.h"
#include "err.h"                         /* DG_OK:回执成功判定 */
#include "bridge/bridge.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdint.h>

/* 最后发出的 op:回执按它配对(页面销毁归 -1,见上) */
static int32_t s_pending_op = -1;

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

/* 确认「清除」:op 经 ud 带入(与多选弹窗的 (void *)(intptr_t) 同款打包) */
static void confirm_pick(void *ud, int idx)
{
    if (idx != 0)
        return;
    int32_t op = (int32_t)(intptr_t)ud;
    s_pending_op = op;               /* 先记后发:回执可能极快回来 */
    bridge_data_op(op);
}

/* 统一的二次确认弹窗:标题区分动作,唯一选项红底(破坏性) */
static void ask_clear(const char *title, int32_t op)
{
    const char *const opts[] = { _("清除") };
    dg_popup_choice_ex(title, opts, 1, 1u << 0, confirm_pick, NULL,
                       (void *)(intptr_t)op);
}

static void on_finger_clear(lv_event_t *e)
{
    (void)e;
    ask_clear(_("确认清除所有指纹?"), DG_DATA_OP_FINGER_CLEAR);
}

static void on_user_clear(lv_event_t *e)
{
    (void)e;
    ask_clear(_("确认清除所有用户?"), DG_DATA_OP_USER_CLEAR);
}

static void on_all_clear(lv_event_t *e)
{
    (void)e;
    ask_clear(_("确认清除所有数据?"), DG_DATA_OP_ALL_CLEAR);
}

static void on_cfg_reset(lv_event_t *e)
{
    (void)e;
    ask_clear(_("确认恢复默认配置?"), DG_DATA_OP_CFG_RESET);
}

void page_device_data_on_result(const ev_sys_data_op_t *ev)
{
    if (ev->op != s_pending_op)
        return;                      /* 陈旧/他页回执:丢弃 */
    s_pending_op = -1;
    if (ev->err == DG_OK)
        dg_popup_success(_("已清除"), 1500, NULL, NULL);
    else
        dg_popup_fail(_("操作失败"), 1500, NULL, NULL);
}

void page_device_data_create(lv_obj_t *parent)
{
    DG_LOGI("[DEVICE_DATA]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("设备数据"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* 破坏性页:顶部一句总告警(逐条确认弹窗只给动作名,后果在这里讲一次) */
    lv_obj_t *warn = lv_label_create(parent);
    lv_label_set_text(warn, _("清除后不可恢复"));
    lv_obj_set_style_text_font(warn, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(warn, DG_COL_ERR(), 0);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 116);

    lv_obj_t *b1 = dg_btn_create_danger(parent, LV_SYMBOL_TRASH,
                                        _("清除所有指纹"));
    lv_obj_set_size(b1, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(b1, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_add_event_cb(b1, on_finger_clear, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = dg_btn_create_danger(parent, LV_SYMBOL_TRASH,
                                        _("清除所有用户"));
    lv_obj_set_size(b2, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(b2, LV_ALIGN_TOP_MID, 0, 180 + DG_BTN_H + DG_PAD);
    lv_obj_add_event_cb(b2, on_user_clear, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b3 = dg_btn_create_danger(parent, LV_SYMBOL_TRASH,
                                        _("清除所有数据"));
    lv_obj_set_size(b3, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(b3, LV_ALIGN_TOP_MID, 0, 180 + 2 * (DG_BTN_H + DG_PAD));
    lv_obj_add_event_cb(b3, on_all_clear, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b4 = dg_btn_create_danger(parent, LV_SYMBOL_REFRESH,
                                        _("恢复默认配置"));
    lv_obj_set_size(b4, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(b4, LV_ALIGN_TOP_MID, 0, 180 + 3 * (DG_BTN_H + DG_PAD));
    lv_obj_add_event_cb(b4, on_cfg_reset, LV_EVENT_CLICKED, NULL);
}

void page_device_data_destroy(void)
{
    s_pending_op = -1;               /* 迟到的回执不再弹到别的页上 */
    DG_LOGI("[DEVICE_DATA]", "page destroy");
}
