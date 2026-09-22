/*
 * page_home.c — 主页视图(spec-ui §3.1):推流画布 + 脸框 + 提示条 + 按钮
 *
 * 分层(pages 层,纯视图):建控件、20ms 刷预览、按钮点击转 bridge 动作;
 * 渲染内容事件经 setter 注入(page_home_set_*),业务在 presenter_home。
 */
#include "page_home.h"
#include "bridge/bridge.h"
#include "dg_log.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"
#include "widgets/dg_preview.h"

#include "modules/display/display.h"
#include "modules/net/net_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t *s_preview = NULL;     /* dg_preview:plane 直通或软渲染回退 */
static lv_obj_t *s_facebox = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_clock = NULL;
static lv_obj_t *s_net = NULL;         /* WiFi 图标(绿=在线/红=离线) */
static lv_timer_t *s_pump_timer = NULL;
static lv_timer_t *s_status_timer = NULL;

/* ---- 相机帧 → 预览(dg_preview 控件,20ms 泵,2026-09-22 video-plane) ----
 * plane 模式:NV12 dma-buf 直送 VOP2 Overlay(zpos 在 UI 之下),控件只是
 * 透明占位,预览像素零 CPU;回退模式:原幅直通软渲染(板测 26~27fps)。
 * 脸框/提示/按钮仍屏幕域坐标,video plane 在 UI 之下照常对齐 */

static void canvas_timer_cb(lv_timer_t *t)
{
    (void)t;
    dg_preview_pump(s_preview);
}

/* ---- 状态栏(左上时钟 + 右上网络;1s 轮询) ---- */

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_clock) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min,
                 tmv.tm_sec);
        lv_label_set_text(s_clock, buf);
    }
    if (s_net) {
        /* 「有网络」= 主接口已拿到 IPv4(面板在网内,web/mDNS/上位机可达)。
         * 只读 getifaddrs,无阻塞——外网可达性(ping 会阻塞)不在这条路径上;
         * 只读直调登记:net_info 是唯一「取哪个 IP」规则的所有者 */
        char ip[64];
        const bool online =
            net_info_primary_ipv4(ip, sizeof(ip)) == DG_OK;
        /* 图标颜色已含全部状态语义(绿=在线/红=离线),不再叠红叉图标 */
        lv_obj_set_style_text_color(s_net, online ? DG_COL_OK() : DG_COL_ERR(), 0);
    }
}

/* ---- 按钮 → bridge 动作 ---- */

static void click_pub(const ev_ui_btn_t *btn)
{
    dg_popup_close();
    bridge_btn(btn);
}

static void on_menu_btn(lv_event_t *e)
{
    (void)e;
    ev_ui_btn_t b = { .btn = DG_BTN_MENU };
    click_pub(&b);
}

static void on_verify_btn(lv_event_t *e)
{
    (void)e;
    ev_ui_btn_t b = { .btn = DG_BTN_VERIFY };
    click_pub(&b);
}

/* ---- 生命周期 ---- */

void page_home_create(lv_obj_t *parent)
{
    DG_LOGI("[HOME]", "page create");

    /* 透明根:video-plane 下未画区域透出下层视频(plane 模式);fb 先清,
     * 清掉导航上一页的不透明残留。软渲染回退模式 LVGL 画不透明预览,无关 */
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    display_clear_fbs();

    s_preview = dg_preview_create(parent, "HOME");

    s_facebox = lv_obj_create(parent);
    lv_obj_remove_style_all(s_facebox);
    lv_obj_set_style_border_width(s_facebox, 4, 0);
    lv_obj_set_style_border_color(s_facebox, DG_COL_WARN(), 0);
    lv_obj_set_style_radius(s_facebox, 8, 0);
    lv_obj_set_style_bg_opa(s_facebox, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);

    /* 提示条:黑色半透明衬底 chip(直接叠在推流上,白字/黄字才可读) */
    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_hint, DG_COL_BG(), 0);
    lv_obj_set_style_bg_color(s_hint, DG_COL_SCRIM(), 0);
    lv_obj_set_style_bg_opa(s_hint, DG_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_hint, 8, 0);
    lv_obj_set_style_pad_hor(s_hint, 14, 0);
    lv_obj_set_style_pad_ver(s_hint, 6, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    /* 状态栏:左上实时时钟,右上网络图标。同款衬底 chip,从推流里「浮」出来 */
    s_clock = lv_label_create(parent);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_clock, DG_COL_BG(), 0);
    lv_obj_set_style_bg_color(s_clock, DG_COL_SCRIM(), 0);
    lv_obj_set_style_bg_opa(s_clock, DG_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_clock, 8, 0);
    lv_obj_set_style_pad_hor(s_clock, 10, 0);
    lv_obj_set_style_pad_ver(s_clock, 4, 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, DG_PAD, DG_PAD);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);

    s_net = lv_label_create(parent);
    lv_label_set_text(s_net, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_net, &lv_font_montserrat_28, 0);
    lv_obj_set_style_bg_color(s_net, DG_COL_SCRIM(), 0);
    lv_obj_set_style_bg_opa(s_net, DG_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_net, 8, 0);
    lv_obj_set_style_pad_all(s_net, 6, 0);
    lv_obj_align(s_net, LV_ALIGN_TOP_RIGHT, -DG_PAD, DG_PAD);
    lv_obj_clear_flag(s_net, LV_OBJ_FLAG_CLICKABLE);

    s_status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
    status_timer_cb(NULL);              /* 创建即显示当前值,不等 1s */

    lv_obj_t *menu_btn = dg_btn_create(parent, LV_SYMBOL_SETTINGS, _("菜单"));
    lv_obj_set_size(menu_btn, 200, DG_BTN_H);
    lv_obj_align(menu_btn, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(menu_btn, on_menu_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *verify_btn = dg_btn_create(parent, LV_SYMBOL_OK, _("验证"));
    lv_obj_set_size(verify_btn, 200, DG_BTN_H);
    lv_obj_align(verify_btn, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(verify_btn, on_verify_btn, LV_EVENT_CLICKED, NULL);

    s_pump_timer = lv_timer_create(canvas_timer_cb, 20, NULL); /* 50Hz 采样:相机
                                                                  30fps 自由跑,泵频高于帧频才不吃拍频损失(seq 去重后空转近乎零成本) */
}

void page_home_destroy(void)
{
    DG_LOGI("[HOME]", "page destroy");
    if (s_pump_timer) {
        lv_timer_del(s_pump_timer);
        s_pump_timer = NULL;
    }
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    dg_preview_destroy(s_preview);
    s_preview = NULL;
    s_facebox = NULL;
    s_hint = NULL;
    s_clock = NULL;
    s_net = NULL;
}

/* ---- setter(presenter 渲染入口) ---- */

void page_home_set_facebox(int state, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s_facebox)
        return;
    lv_color_t c = (state == DG_BOX_MATCHED)  ? DG_COL_OK()
                   : (state == DG_BOX_FAILED) ? DG_COL_ERR()
                                              : DG_COL_WARN();
    lv_obj_clear_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_facebox, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_size(s_facebox, (lv_coord_t)w, (lv_coord_t)h);
    lv_obj_set_style_border_color(s_facebox, c, 0);
    lv_obj_invalidate(s_facebox);
}

/* 只改颜色不挪位置:FSM 命中/失败时框位置沿用检测框(框由视觉后端逐帧刷新) */
void page_home_set_facebox_color(int state)
{
    if (!s_facebox)
        return;
    lv_color_t c = (state == DG_BOX_MATCHED)  ? DG_COL_OK()
                   : (state == DG_BOX_FAILED) ? DG_COL_ERR()
                                              : DG_COL_WARN();
    lv_obj_set_style_border_color(s_facebox, c, 0);
    lv_obj_invalidate(s_facebox);
}

void page_home_clear_facebox(void)
{
    if (s_facebox)
        lv_obj_add_flag(s_facebox, LV_OBJ_FLAG_HIDDEN);
}

void page_home_set_hint(const char *text)
{
    if (!s_hint)
        return;
    if (!text) {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_hint, text);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}
