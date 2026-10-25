/*
 * ui.c — UI 子系统引导(装配顺序学自 ESP32 ovs ui_bootstrap)
 *
 *   display → i18n → navigator(绑根) → 注册页面 → bridge(事件桥)
 *   → 全局事件泵(33ms,页面生命周期之外)→ 压栈首页
 *
 * 事件流向:
 *   后端 event_bus → bridge(入队) → 本泵(LVGL 线程) → navigator
 *   ├─ UI_EVT_GOTO_PAGE → navigator_switch(切页)
 *   └─ 其余            → navigator_dispatch_evt(当前页 presenter 渲染)
 */
#include "ui.h"
#include "cfg.h"
#include "modules/display/display.h"
#include "fp_sync.h"
#include "i18n.h"
#include "lvgl.h"
#include "navigator/navigator.h"
#include "bridge/bridge.h"
#include "port.h"
#include "theme.h"
#include "ui_events.h"
#include "widgets/dg_avatar.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>

extern void presenter_home_register(void);
extern void presenter_standby_register(void);
extern void presenter_menu_register(void);
extern void presenter_users_register(void);
extern void presenter_user_edit_register(void);
extern void presenter_capture_register(void);
extern void presenter_device_register(void);
extern void presenter_display_set_register(void);
extern void presenter_access_set_register(void);
extern void presenter_logs_register(void);
extern void presenter_web_set_register(void);
extern void presenter_net_set_register(void);
extern void presenter_face_set_register(void);
extern void presenter_finger_set_register(void);
extern void presenter_about_register(void);
extern void presenter_device_data_register(void);

static lv_obj_t *s_page_root = NULL;

/* ---- 动作活体顶层提示条(B8)----
 * 挂 lv_layer_top:主页/验证弹窗/管理员认证页都要能看到「请点头」这类指令
 * (活体是认证管线强制阶段,提示被页面挡住 = 用户不知道做什么)。与页面
 * 生命周期无关,由 EV_LIVENESS_PROMPT 的 STEP/PASSED/HIDE 三相驱动显隐 */
static lv_obj_t *s_live_chip;
static lv_obj_t *s_live_lbl;

static void liveness_chip_create(void)
{
    s_live_lbl = lv_label_create(lv_layer_top());
    lv_label_set_text(s_live_lbl, "");
    lv_obj_set_style_text_font(s_live_lbl, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_live_lbl, DG_COL_BG(), 0);
    lv_obj_set_style_bg_color(s_live_lbl, DG_COL_SCRIM(), 0);
    lv_obj_set_style_bg_opa(s_live_lbl, DG_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_live_lbl, DG_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_live_lbl, 14, 0);
    lv_obj_set_style_pad_ver(s_live_lbl, 10, 0);
    lv_obj_align(s_live_lbl, LV_ALIGN_BOTTOM_MID, 0, -150);
    lv_obj_clear_flag(s_live_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_live_lbl, LV_OBJ_FLAG_HIDDEN);
    s_live_chip = s_live_lbl;            /* label 自带底色,无需再包容器 */
}

static void liveness_chip_apply(const ev_liveness_t *lv)
{
    if (!s_live_chip)
        return;
    if (lv->phase == DG_LIVE_PHASE_STEP) {
        char txt[64];
        snprintf(txt, sizeof(txt), "%s %d/%d",
                 lv->action == DG_LIVE_ACTION_NOD ? _("请点头") : _("请摇头"),
                 lv->step, lv->total);
        lv_label_set_text(s_live_lbl, txt);
        lv_obj_clear_flag(s_live_chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_live_chip, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 开机指纹数量一致性检查的告知(fp_sync,2026-10-11):检查在 provider 线程
 * 里跑完,可能早于 UI 就绪;event_bus 不回放,所以这里拉取式补弹——
 * 取走即清,一次检查只弹一次。 */
static void fp_notice_poll(void)
{
    int32_t n = 0;
    if (!bridge_fp_notice_take(&n))
        return;
    switch (n) {
    case FP_NOTICE_MISMATCH:
        dg_popup_fail(_("指纹数量出错,建议清除所有指纹"), 5000, NULL, NULL);
        break;
    case FP_NOTICE_CLEARED_MODULE:
        dg_popup_success(_("指纹数量不一致,已清除模组内指纹"), 4000, NULL, NULL);
        break;
    case FP_NOTICE_CLEARED_DB:
        dg_popup_fail(_("指纹数量不一致,已清除设备端指纹记录"), 4000, NULL, NULL);
        break;
    case FP_NOTICE_READ_FAIL:
        dg_popup_fail(_("指纹模块读取异常,未校验指纹数量"), 4000, NULL, NULL);
        break;
    default:
        break;
    }
}

/* 全局事件泵:必须活在页面生命周期之外——
 * 原实现泵在 page_home 的定时器里,进待机 → home 销毁 → 泵停 →
 * 唤醒事件永远无人处理,屏幕卡死在待机(实测) */
static void ui_evt_pump_cb(lv_timer_t *t)
{
    (void)t;
    ui_evt_t evt;
    while (ui_evt_pop(&evt)) {
        if (evt.kind == UI_EVT_GOTO_PAGE) {
            dg_popup_close();               /* 弹窗属当前页,切页即收(top layer 不随页销毁) */
            navigator_switch(evt.page.page); /* 栈内回退/平级切换 */
        } else if (evt.kind == UI_EVT_LIVENESS) {
            liveness_chip_apply(&evt.liveness); /* 顶层提示条,页面之外(见上) */
        } else {
            /* 录入结果终态的头像缓存集中失效:web 上传(与任何未来的录入
             * 路径)改 DB 头像时设备端没有任何页面在场,各页面自己的
             * invalidate 都顾不到;这里在 UI 线程统一兜底(缓存失效近零
             * 开销,误失效只是下次 get 重解码一次)。FACE_OK 也失效是刻意
             * 的:该事件同时承载「草稿就绪」(设备拍摄)与「直落库」(web),
             * UI 层无从区分,按「可能变了」处理最稳 */
            if (evt.kind == UI_EVT_ENROLL_RESULT &&
                evt.enroll.err == DG_OK &&
                (evt.enroll.kind == DG_ENROLL_FACE ||
                 evt.enroll.kind == DG_ENROLL_FACE_CLEAR))
                dg_avatar_invalidate(evt.enroll.user_id);
            navigator_dispatch_evt(&evt);    /* 内容事件给当前页渲染 */
        }
    }
    fp_notice_poll();                        /* 开机一致性检查的补弹(拉取式) */
}

int ui_init(const dg_ui_args_t *args)
{
    lv_init();
    int rc = display_init();
    if (rc != DG_OK)
        return rc;
    /* v9 时基:lv_tick_set_cb 注入(8.3 经 LV_TICK_CUSTOM)。必须在
     * display_init 之后:v9 DRM 驱动 create 时会先装自己的时基 */
    lv_tick_set_cb(dg_ui_tick_ms);

    const char *lang = cfg_get()->language;
    i18n_init(args ? args->lang_dir : "lang");

    /* 显式套用配置语言(init 默认 zh-CN) */
    if (lang && strcmp(lang, "zh-CN") != 0)
        i18n_set_language(lang);

    s_page_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_page_root);
    lv_obj_set_size(s_page_root, DG_SCREEN_W, DG_SCREEN_H);
    navigator_init(s_page_root);

    presenter_home_register();
    presenter_standby_register();
    presenter_menu_register();
    presenter_users_register();
    presenter_user_edit_register();
    presenter_capture_register();
    presenter_device_register();
    presenter_display_set_register();
    presenter_access_set_register();
    presenter_logs_register();
    presenter_web_set_register();
    presenter_net_set_register();
    presenter_face_set_register();
    presenter_finger_set_register();
    presenter_about_register();
    presenter_device_data_register();

    bridge_init();
    liveness_chip_create();              /* 顶层提示条在 bridge 之后(事件随到随渲染) */
    /* 触摸按下沿 → EV_UI_TOUCH:任何页面的触摸都算「有操作」
     * (清待机/菜单无操作计数;此前只有待机页自己发,菜单里摸屏不算操作) */
    display_set_touch_listener(bridge_touch);
    navigator_push("home");
    lv_timer_create(ui_evt_pump_cb, 33, NULL); /* 事件泵 30fps 跟手 */

    return DG_OK;
}

void ui_poll(void)
{
    display_poll();
    lv_timer_handler();
}
