/*
 * page_finger_set.c — 指纹管理页(2026-10-03 用户反馈重做录入 UI)
 *
 * 编辑页「指纹」行进入。三个槽位(指纹一/二/三)= 该用户已录指纹的
 * 位置序(第 N 枚;模组 PageID 全局分配,槽位只是本用户内的呈现序)。
 * 已录槽位给「删除」;空槽位都给「录入」——模组侧只能追加,从任意空槽
 * 发起的新指纹一律落在最靠前的空槽(呈现序),录入不必按槽位顺序
 * (2026-10-04 用户口径:三枚各自独立,不要求先录一才能录二三)。
 *
 * 录入引导窗为本页专用浮层(非 dg_popup):指纹图标 + 大字提示 +
 * 「等按压」阶段 5s 无按压自动退出(超时发 CANCEL,provider 在检查点
 * 回滚未落库模板),过程文案映射 EV_ENROLL_PROGRESS 的 step;LIFT/PROCESS
 * 两个「不等输入」阶段停表(处理尾巴慢 ≠ 没按压,计时不停会在真终态前
 * 误弹「已退出录入」= 成功却先见失败窗,2026-10-04);终态 EV_ENROLL_RESULT
 * 关窗。页面销毁(返回/被盖)一律撤消进行中的录入流,防流程悬挂。
 *
 * 分层:纯视图 + 页内小状态机;动作经 bridge,数据经 enroll_service 快照。
 */
#include "page_finger_set.h"
#include "bridge/bridge.h"
#include "dg_log.h"
#include "enroll_service.h"
#include "err.h"
#include "events.h"
/* 相对路径而非裸名:dg_ui 未链接 fp 目标,include path 无 fingerprint 目录
 * (裸名只有 main 可执行目标用;这里只要 ready 声明,不值得为此动依赖图) */
#include "services/verify/fingerprint/fp_provider.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "valid_ui.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SLOT_N        DG_FINGER_PAGES_MAX
#define PRESS_TIMEOUT 5000                /* 每阶段无按压 5s 退出(用户口径) */

static char s_uid[DG_UID_LEN];

static lv_obj_t *s_val[SLOT_N];           /* 槽位状态文本 */
static lv_obj_t *s_btn[SLOT_N];           /* 槽位动作按钮(录入/删除/隐藏) */
static int32_t   s_pages[SLOT_N];         /* 已录槽位的 PageID(删除用) */
static uint32_t  s_cnt;                   /* 已录枚数 */

/* 录入引导窗(页内浮层) */
static lv_obj_t *s_overlay;               /* 全页衬底:显示 = 录入进行中 */
static lv_obj_t *s_ov_title;              /* 大字阶段提示 */
static lv_obj_t *s_ov_hint;               /* 小字:超时说明/重按原因 */
static lv_timer_t *s_press_tmr;
static bool      s_enrolling;
static uint32_t  s_seq;                   /* 本次录入请求 seq(回执配对) */

static void refresh_slots(void);

/* 槽位行(标题静态,值/按钮随库刷新) */
static const char *slot_title(int i)
{
    switch (i) {
    case 0: return _("指纹一");
    case 1: return _("指纹二");
    default: return _("指纹三");
    }
}

/* ---- 引导窗 ---- */

/* 指纹图标:同心弧圈(LVGL arc 拼出,零图片资源;蓝白主题主蓝) */
static lv_obj_t *fp_icon_create(lv_obj_t *parent, lv_coord_t y)
{
    static const int16_t dia[5] = { 190, 150, 110, 70, 34 };
    static const int16_t span[5] = { 300, 250, 280, 220, 360 };
    static const int16_t rot[5] = { 30, 200, 120, 330, 0 };

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 200, 200);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 5; i++) {
        lv_obj_t *arc = lv_arc_create(box);
        lv_obj_remove_style_all(arc);     /* 干掉主题底/边,只留指示弧 */
        lv_obj_set_size(arc, dia[i], dia[i]);
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(arc, rot[i]);
        lv_arc_set_bg_angles(arc, 0, span[i]);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 100);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(DG_COLOR_PRIMARY),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    }
    return box;
}

static void press_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_press_tmr = NULL;
    if (!s_enrolling)
        return;
    /* 5s 无按压退出:撤消流程(未 Store 模板由 provider 检查点回滚) */
    bridge_enroll_request(s_uid, DG_ENROLL_FINGER_CANCEL);
    s_enrolling = false;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    /* 未按下的中性提示走醒目样式短提示(编辑页既有惯例),非「验证失败」语义 */
    dg_popup_fail(_("未检测到指纹，已退出录入"), 1800, NULL, NULL);
}

static void press_timer_arm(void)
{
    if (s_press_tmr)
        lv_timer_delete(s_press_tmr);
    s_press_tmr = lv_timer_create(press_timer_cb, PRESS_TIMEOUT, NULL);
    lv_timer_set_repeat_count(s_press_tmr, 1);
}

static void press_timer_stop(void)
{
    if (s_press_tmr) {
        lv_timer_delete(s_press_tmr);
        s_press_tmr = NULL;
    }
}

/* 阶段提示(step → 文案)。「等按压」阶段重开 5s 计时(新阶段重新计按压);
 * LIFT/PROCESS 是「不等输入」阶段(provider 在处理/等释放),停表——
 * 计时器只在真的该等按压时才计,否则处理尾巴一慢就先弹退出误报 */
static void overlay_stage(int32_t step)
{
    const char *title = _("请按指纹");
    const char *hint = _("5秒内无按压将自动退出");

    switch (step) {
    case DG_ENROLL_FP_STEP_PRESS1:
        break;
    case DG_ENROLL_FP_STEP_PRESS2:
        title = _("请再按一次指纹");
        break;
    case DG_ENROLL_FP_STEP_RETRY2:
        title = _("请再按一次指纹");
        hint = _("两次按压指纹不一致，请用同一手指");
        break;
    case DG_ENROLL_FP_STEP_QUALITY:
        title = _("请再按一次指纹");
        hint = _("未读到指纹，请调整手指贴合传感器");
        break;
    case DG_ENROLL_FP_STEP_LIFT:
        title = _("请抬起手指");
        hint = _("松开手指后再进行第二次按压");
        press_timer_stop();
        lv_label_set_text(s_ov_title, title);
        lv_label_set_text(s_ov_hint, hint);
        return;
    case DG_ENROLL_FP_STEP_PROCESS:
        title = _("正在录入，请稍候");
        hint = "";
        press_timer_stop();
        lv_label_set_text(s_ov_title, title);
        lv_label_set_text(s_ov_hint, hint);
        return;
    default:
        break;
    }
    lv_label_set_text(s_ov_title, title);
    lv_label_set_text(s_ov_hint, hint);
    press_timer_arm();
}

static void enroll_begin(void)
{
    if (s_enrolling)
        return;
    /* 模组不在位(串口没接/握手未过/掉线降级)入口即拒:否则引导窗弹
     * 「请按压」后永远等不到任何回执。文案对齐主页/验证侧(reason=9) */
    if (!fp_provider_ready()) {
        dg_popup_fail(_("指纹模块未就绪"), 1500, NULL, NULL);
        return;
    }
    s_seq = bridge_enroll_request(s_uid, DG_ENROLL_FINGER);
    s_enrolling = true;
    overlay_stage(DG_ENROLL_FP_STEP_PRESS1);   /* 文案先上屏,进度随后对齐 */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* 终态/超时/取消共用的收尾:停表 + 收窗 + 刷槽位 */
static void enroll_finish(void)
{
    press_timer_stop();
    s_enrolling = false;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    refresh_slots();
}

static void on_overlay_cancel(lv_event_t *e)
{
    (void)e;
    if (!s_enrolling)
        return;
    bridge_enroll_request(s_uid, DG_ENROLL_FINGER_CANCEL);
    enroll_finish();
}

/* ---- 槽位 ---- */

static void refresh_slots(void)
{
    if (enroll_service_finger_pages(s_uid, s_pages, SLOT_N, &s_cnt) != DG_OK)
        s_cnt = 0;

    for (int i = 0; i < SLOT_N; i++) {
        if (!s_val[i])
            continue;
        if ((uint32_t)i < s_cnt) {
            lv_label_set_text(s_val[i], _("已录入"));
            dg_btn_set_label(s_btn[i], _("删除"));
            lv_obj_clear_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            /* 空槽都可录:不强制顺序;新指纹落在最靠前空槽(呈现序,模组
             * 只能追加),录入完成后本页刷新即见真位次(2026-10-04) */
            lv_label_set_text(s_val[i], _("未录入"));
            dg_btn_set_label(s_btn[i], _("录入"));
            lv_obj_clear_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void apply_del(void *ud, int idx)
{
    (void)idx;                            /* 单选项红色确认弹窗:仅「删除」 */
    int slot = (int)(intptr_t)ud;
    if (slot < 0 || slot >= SLOT_N || (uint32_t)slot >= s_cnt)
        return;
    bridge_enroll_request_arg(s_uid, DG_ENROLL_FINGER_DEL, s_pages[slot]);
}

static void on_slot_btn(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int slot = -1;
    for (int i = 0; i < SLOT_N; i++)
        if (s_btn[i] == btn)
            slot = i;
    if (slot < 0 || s_enrolling)
        return;

    if ((uint32_t)slot < s_cnt) {
        /* 单选项红色确认弹窗(破坏性;模式同编辑页删用户) */
        const char *const del_opts[] = { _("删除") };
        dg_popup_choice_ex(_("确认删除该指纹"), del_opts, 1, 1u << 0,
                           apply_del, NULL, (void *)(intptr_t)slot);
        return;
    }
    enroll_begin();                       /* 任意空槽:追加录入(落最前空槽) */
}

/* ---- 事件(LVGL 线程;bridge 入队 → navigator 派发到当前页) ---- */

void page_finger_set_evt(const ui_evt_t *evt)
{
    if (evt->kind == UI_EVT_ENROLL_PROGRESS) {
        const ev_enroll_progress_t *p = &evt->progress;
        if (!s_enrolling || p->kind != DG_ENROLL_FINGER ||
            strcmp(p->user_id, s_uid) != 0 || p->seq != s_seq)
            return;
        overlay_stage(p->step);
        return;
    }
    if (evt->kind != UI_EVT_ENROLL_RESULT)
        return;
    const ev_enroll_result_t *r = &evt->enroll;
    if (r->kind != DG_ENROLL_FINGER && r->kind != DG_ENROLL_FINGER_DEL)
        return;
    if (strcmp(r->user_id, s_uid) != 0)
        return;

    if (r->kind == DG_ENROLL_FINGER_DEL) {
        dg_popup_success(_("已删除"), 800, NULL, NULL);
        refresh_slots();
        return;
    }

    if (!s_enrolling) {
        /* 无在途录入:只认「同 seq 的迟到成功」——录入确实落了库必须告知
         * 并刷新槽位;迟到失败(旧 seq 残回执/取消后报错)一律不弹,否则
         * 就是「明明成功却先见失败窗」的另一半来源(2026-10-04) */
        if (r->seq == s_seq && r->err == DG_OK) {
            dg_popup_success(_("指纹已录入"), 1200, NULL, NULL);
            refresh_slots();
        }
        return;
    }
    if (r->seq != s_seq)
        return;

    enroll_finish();
    if (r->err == DG_OK)
        dg_popup_success(_("指纹已录入"), 1200, NULL, NULL);
    else if (r->err == DG_ERR_DUP_FINGER)
        dg_popup_fail(_("该指纹已录入过，请更换手指"), 2000, NULL, NULL);
    else
        dg_popup_fail(dg_ui_enroll_err_text(r->err), 2000, NULL, NULL);
}

/* ---- 页面装配 ---- */

static lv_obj_t *slot_row(lv_obj_t *parent, int i, lv_coord_t y)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, DG_SCREEN_W - 2 * DG_PAD, 96);
    lv_obj_set_pos(row, DG_PAD, y);
    lv_obj_set_style_bg_color(row, DG_COL_BG_LIGHT(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, DG_RADIUS, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, DG_COL_BG(), 0);
    lv_obj_set_style_border_opa(row, DG_OPA_CARD_LINE, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, slot_title(i));
    lv_obj_set_style_text_font(lbl, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(lbl, DG_COL_TEXT(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_TEXT(), 0);
    lv_obj_align(val, LV_ALIGN_LEFT_MID, 190, 0);
    s_val[i] = val;

    lv_obj_t *btn = dg_btn_create_light(row, NULL, "");
    lv_obj_set_size(btn, 150, 72);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(btn, on_slot_btn, LV_EVENT_CLICKED, NULL);
    s_btn[i] = btn;
    return row;
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (s_enrolling) {
        bridge_enroll_request(s_uid, DG_ENROLL_FINGER_CANCEL);
        enroll_finish();
    }
    navigator_back();
}

void page_finger_set_open(const char *uid)
{
    snprintf(s_uid, sizeof(s_uid), "%s", uid ? uid : "");
}

void page_finger_set_create(lv_obj_t *parent)
{
    DG_LOGI("[FINGER_SET]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("指纹管理"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tip = lv_label_create(parent);
    lv_label_set_text(tip, _("每人最多可录 3 枚指纹"));
    lv_obj_set_style_text_font(tip, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(tip, DG_COL_TEXT(), 0);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 96);

    for (int i = 0; i < SLOT_N; i++)
        slot_row(parent, i, 170 + i * 116);
    refresh_slots();

    /* 录入引导窗:全页衬底 + 白卡片(图标/大字/小字/取消),初始隐藏 */
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_style_bg_color(s_overlay, DG_COL_SCRIM(), 0);
    lv_obj_set_style_bg_opa(s_overlay, DG_OPA_SCRIM, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 560, 760);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, DG_RADIUS, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, DG_COL_BG(), 0);
    lv_obj_set_style_border_opa(card, DG_OPA_CARD_LINE, 0);

    fp_icon_create(card, 56);

    s_ov_title = lv_label_create(card);
    lv_obj_set_style_text_font(s_ov_title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_ov_title, DG_COL_TEXT(), 0);
    lv_obj_align(s_ov_title, LV_ALIGN_TOP_MID, 0, 300);

    s_ov_hint = lv_label_create(card);
    lv_obj_set_style_text_font(s_ov_hint, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_ov_hint, DG_COL_TEXT(), 0);
    lv_label_set_long_mode(s_ov_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ov_hint, 480);
    lv_obj_align(s_ov_hint, LV_ALIGN_TOP_MID, 0, 380);

    lv_obj_t *btn_cancel = dg_btn_create_light(card, NULL, _("取消"));
    lv_obj_set_size(btn_cancel, 220, 88);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(btn_cancel, on_overlay_cancel, LV_EVENT_CLICKED, NULL);
}

void page_finger_set_destroy(void)
{
    DG_LOGI("[FINGER_SET]", "page destroy");
    /* 页面销毁即撤消录入流(未落库模板由 provider 回滚),防流程悬挂 */
    if (s_enrolling)
        bridge_enroll_request(s_uid, DG_ENROLL_FINGER_CANCEL);
    press_timer_stop();
    s_enrolling = false;
    s_overlay = NULL;
    s_ov_title = NULL;
    s_ov_hint = NULL;
    for (int i = 0; i < SLOT_N; i++) {
        s_val[i] = NULL;
        s_btn[i] = NULL;
    }
    s_cnt = 0;
}
