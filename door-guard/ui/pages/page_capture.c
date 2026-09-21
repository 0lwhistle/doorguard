/*
 * page_capture.c — 人脸拍摄录入页(2026-09-21 拍摄录入交接 §3.2)
 *
 * 用户在编辑页点「人脸·录入」进本页:实时预览 + 质量实时提示 +
 * [拍摄][取消]。只有质量合格(清晰/够大/检测分够)才允许拍;
 * 拍下后回看刚入库的照片,可 [重拍] 或 [完成] 返回编辑页。
 *
 * 分层(pages 层,纯视图 + 页内小状态机):
 *   预览    camera_latest → lv_canvas(与主页同一套,33ms 刷新)
 *   质量    UI_EVT_QUALITY(verdict → 具体文案/颜色,控制拍摄使能)
 *   拍摄    EV_ENROLL_REQUEST(DG_ENROLL_FACE)→ 等 EV_ENROLL_RESULT(5s 超时)
 *   回看    dg_avatar_get(FULL)——照片已同帧入库,从库里读回来最可信
 * 质量判定枚举来自 services/vision/face_quality.h(只读契约:纯枚举,
 * 与事件的 verdict 字段同源,不引入业务调用)。
 */
#include "dg_log.h"
#include "err.h"
#include "event_bus.h"
#include "events.h"
#include "face_quality.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "storage.h"
#include "theme.h"
#include "widgets/dg_avatar.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include "modules/camera/camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char s_uid[DG_UID_LEN];

static lv_obj_t *s_canvas = NULL;
static lv_color_t *s_canvas_buf = NULL;
static lv_obj_t *s_facebox = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_photo = NULL;          /* 回看照片(白底描边容器内) */
static lv_obj_t *s_btn_shot, *s_btn_cancel, *s_btn_retake, *s_btn_done;
static lv_timer_t *s_pump_timer = NULL;
static lv_timer_t *s_wait_timer = NULL;

/* 页内状态:LIVE(取景)→ WAIT(已拍等回执)→ REVIEW(回看)/ 退回 LIVE */
static bool s_review;
static bool s_shot_enabled;               /* 质量合格才可拍 */

static void set_state_live(void);

/* ---- 预览(与 page_home 同一套:camera_latest → canvas,33ms) ---- */

static void canvas_timer_cb(lv_timer_t *t)
{
    (void)t;
    const camera_frame_t *f = camera_latest();
    if (!f || !s_canvas)
        return;
    int32_t w = f->w > DG_SCREEN_W ? DG_SCREEN_W : f->w;
    int32_t h = f->h > DG_SCREEN_H ? DG_SCREEN_H : f->h;
    if (!s_canvas_buf) {
        s_canvas_buf = malloc((size_t)w * h * sizeof(lv_color_t));
        if (!s_canvas_buf)
            return;
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, w, h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
    }
    if (f->w == w && f->h == h) {
        lv_canvas_copy_buf(s_canvas, (const lv_color_t *)f->pixels, 0, 0, w, h);
        lv_obj_invalidate(s_canvas);
    }
}

/* ---- 质量提示 ---- */

static void hint_set(const char *text, lv_color_t col)
{
    lv_label_set_text(s_hint, text);
    lv_obj_set_style_text_color(s_hint, col, 0);
}

static void quality_apply(int verdict)
{
    s_shot_enabled = (verdict == FQ_OK);
    if (verdict == FQ_OK)
        hint_set(_("可以拍摄"), DG_COL_OK());
    else if (verdict == FQ_ERR_SMALL)
        hint_set(_("请靠近一些"), DG_COL_WARN());
    else if (verdict == FQ_ERR_BLURRY)
        hint_set(_("太模糊,请保持不动"), DG_COL_WARN());
    else                                 /* LOW_SCORE:侧脸/遮挡多半也是这个 */
        hint_set(_("请正对摄像头"), DG_COL_WARN());

    if (s_btn_shot) {
        if (s_shot_enabled)
            lv_obj_clear_state(s_btn_shot, LV_STATE_DISABLED);
        else
            lv_obj_add_state(s_btn_shot, LV_STATE_DISABLED);
    }
}

/* ---- 录入回执 5s 超时(链路任何一环无响应时明确告知,不无声) ---- */

static void wait_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_wait_timer = NULL;
    dg_popup_fail(_("录入超时,请正对摄像头重试"), 3000, NULL, NULL);
    set_state_live();
}

static void wait_start(void)
{
    if (s_wait_timer)
        lv_timer_del(s_wait_timer);
    s_wait_timer = lv_timer_create(wait_timeout_cb, 5000, NULL);
    lv_timer_set_repeat_count(s_wait_timer, 1);
}

static void wait_cancel(void)
{
    if (s_wait_timer) {
        lv_timer_del(s_wait_timer);
        s_wait_timer = NULL;
    }
}

/* ---- 状态切换 ---- */

static void set_state_live(void)
{
    s_review = false;
    if (s_photo)
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    if (s_facebox)
        lv_obj_clear_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_shot && s_btn_cancel) {
        lv_obj_clear_flag(s_btn_shot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_retake && s_btn_done) {
        lv_obj_add_flag(s_btn_retake, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_done, LV_OBJ_FLAG_HIDDEN);
    }
    /* 回到取景:质量状态未知,按「无合格脸」处理,等下一个质量事件解锁 */
    s_shot_enabled = false;
    if (s_btn_shot)
        lv_obj_add_state(s_btn_shot, LV_STATE_DISABLED);
    hint_set(_("请正对摄像头"), DG_COL_WARN());
}

static void set_state_review(void)
{
    s_review = true;
    if (s_facebox)
        lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_shot && s_btn_cancel) {
        lv_obj_add_flag(s_btn_shot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_retake && s_btn_done) {
        lv_obj_clear_flag(s_btn_retake, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_done, LV_OBJ_FLAG_HIDDEN);
    }
    hint_set(_("已拍摄"), DG_COL_OK());

    /* 刚入库的照片从 DB 读回(dg_avatar 缓存已失效,必定是新图) */
    const lv_img_dsc_t *dsc = dg_avatar_get(s_uid, DG_AVATAR_FULL);
    if (dsc && s_photo) {
        lv_img_set_src(s_photo, dsc);
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    } else if (s_photo) {
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- 按钮 ---- */

static void on_shot(lv_event_t *e)
{
    (void)e;
    if (!s_shot_enabled || s_review)
        return;
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", s_uid);
    ev.kind = DG_ENROLL_FACE;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
    wait_start();
    lv_obj_add_state(s_btn_shot, LV_STATE_DISABLED);
}

static void on_cancel(lv_event_t *e)
{
    (void)e;
    wait_cancel();
    navigator_back();
}

static void on_retake(lv_event_t *e)
{
    (void)e;
    set_state_live();
}

static void on_done(lv_event_t *e)
{
    (void)e;
    wait_cancel();
    navigator_back();                    /* 编辑页重建即刷新(人脸=已录入+头像) */
}

/* ---- 事件(LVGL 线程,经 bridge 入队) ---- */

static void on_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_FACE_BOX:
        if (!s_review && s_facebox) {
            lv_obj_clear_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_facebox, evt->box.x, evt->box.y);
            lv_obj_set_size(s_facebox, evt->box.w, evt->box.h);
        }
        break;
    case UI_EVT_FACE_LOST:
        if (!s_review) {
            if (s_facebox)
                lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
            quality_apply(FQ_ERR_LOW_SCORE);   /* 无脸 = 提示正对镜头,禁拍 */
        }
        break;
    case UI_EVT_QUALITY:
        if (!s_review)
            quality_apply(evt->quality.verdict);
        break;
    case UI_EVT_ENROLL_RESULT:
        if (evt->enroll.kind != DG_ENROLL_FACE ||
            strcmp(evt->enroll.user_id, s_uid) != 0)
            break;
        wait_cancel();
        if (evt->enroll.err == DG_OK) {
            dg_avatar_invalidate(s_uid); /* 新照片,缓存作废 */
            set_state_review();
        } else {
            dg_popup_fail(_("操作失败"), 2000, NULL, NULL);
            set_state_live();
        }
        break;
    default:
        break;
    }
}

void page_capture_evt(const ui_evt_t *evt)
{
    on_evt(evt);
}

/* ---- 生命周期 ---- */

void page_capture_open(const char *uid)
{
    snprintf(s_uid, sizeof(s_uid), "%s", uid ? uid : "");
}

void page_capture_create(lv_obj_t *parent)
{
    DG_LOGI("[CAPTURE]", "page create");

    s_canvas = lv_canvas_create(parent);

    s_facebox = lv_obj_create(parent);
    lv_obj_remove_style_all(s_facebox);
    lv_obj_set_style_border_width(s_facebox, 4, 0);
    lv_obj_set_style_border_color(s_facebox, DG_COL_WARN(), 0);
    lv_obj_set_style_radius(s_facebox, 8, 0);
    lv_obj_set_style_bg_opa(s_facebox, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("人脸录入"));
    lv_obj_set_style_text_font(title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint, DG_FONT_CN, 0);
    /* 质量提示同样衬在预览画面上:黑底 chip(颜色按判定切换) */
    lv_obj_set_style_bg_color(s_hint, DG_COL_SCRIM(), 0);
    lv_obj_set_style_bg_opa(s_hint, DG_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_hint, 8, 0);
    lv_obj_set_style_pad_hor(s_hint, 14, 0);
    lv_obj_set_style_pad_ver(s_hint, 6, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 64);

    /* 回看照片:白底描边小窗,盖在预览之上(160×160 原尺寸) */
    s_photo = lv_img_create(parent);
    lv_obj_set_style_border_width(s_photo, 4, 0);
    lv_obj_set_style_border_color(s_photo, DG_COL_OK(), 0);
    lv_obj_set_style_radius(s_photo, 8, 0);
    lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, -80);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_btn_shot = dg_btn_create(parent, LV_SYMBOL_OK, _("拍摄"));
    lv_obj_set_size(s_btn_shot, 220, DG_BTN_H);
    lv_obj_align(s_btn_shot, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(s_btn_shot, on_shot, LV_EVENT_CLICKED, NULL);

    s_btn_cancel = dg_btn_create_light(parent, NULL, _("取消"));
    lv_obj_set_size(s_btn_cancel, 220, DG_BTN_H);
    lv_obj_align(s_btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(s_btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);

    s_btn_retake = dg_btn_create(parent, LV_SYMBOL_REFRESH, _("重拍"));
    lv_obj_set_size(s_btn_retake, 220, DG_BTN_H);
    lv_obj_align(s_btn_retake, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(s_btn_retake, on_retake, LV_EVENT_CLICKED, NULL);

    s_btn_done = dg_btn_create(parent, LV_SYMBOL_OK, _("完成"));
    lv_obj_set_size(s_btn_done, 220, DG_BTN_H);
    lv_obj_align(s_btn_done, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(s_btn_done, on_done, LV_EVENT_CLICKED, NULL);

    s_pump_timer = lv_timer_create(canvas_timer_cb, 33, NULL);
    set_state_live();
}

void page_capture_destroy(void)
{
    DG_LOGI("[CAPTURE]", "page destroy");
    if (s_pump_timer) {
        lv_timer_del(s_pump_timer);
        s_pump_timer = NULL;
    }
    wait_cancel();
    if (s_canvas_buf) {
        free(s_canvas_buf);
        s_canvas_buf = NULL;
    }
    s_canvas = NULL;
    s_facebox = NULL;
    s_hint = NULL;
    s_photo = NULL;
    s_btn_shot = s_btn_cancel = s_btn_retake = s_btn_done = NULL;
}
