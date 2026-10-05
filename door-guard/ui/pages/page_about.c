/*
 * page_about.c — 关于设备页(设备管理 → 关于设备;2026-10-05 MQTT OTA)
 *
 * 展示:设备名称 / 固件版本 / 构建日期;升级区:最新版本 + 发布日期 +
 * 更新说明 + 状态行(下载进度/暂存就绪/失败原因)。
 * 交互:「检查更新」走 MQTT 查询(未连接置灰);「立即更新」在有新版且
 * 自动更新关时出现;「自动更新」开关落 cfg。
 * 数据流:进页先拉 ota_update_status 快照,后续 EV_NET_OTA_UPDATE /
 * EV_MQTT_STATE 经 UI 事件泵到 on_result 刷新;STAGED 后 S60 装槽重启
 * (应用会退出,页面无需再弹确认)。
 */
#include "cfg.h"
#include "events.h"
#include "dg_log.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "ota/ota_update.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdio.h>

#ifndef DG_BUILD_DATE
#define DG_BUILD_DATE "-"
#endif

static lv_obj_t *s_lb_ver, *s_lb_date;       /* 当前版本 / 构建日期(值列) */static lv_obj_t *s_lb_new_ver, *s_lb_new_date, *s_lb_notes;
static lv_obj_t *s_lb_status;                /* 升级状态行 */
static lv_obj_t *s_btn_check, *s_btn_apply;  /* 检查更新 / 立即更新 */
static lv_obj_t *s_sw_auto;                  /* 自动更新开关 */
static bool s_mqtt_ok;
static bool s_staged_toast_shown;            /* STAGED 弹窗只发一次 */

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

/* 错误码短文案(查不到的落错误码数字,不猜语义) */
static void fmt_err(char *buf, size_t cap, int err)
{
    switch (err) {
    case DG_ERR_NETWORK:    snprintf(buf, cap, "%s", _("网络未连接"));    break;
    case DG_ERR_TIMEOUT:    snprintf(buf, cap, "%s", _("下载超时"));      break;
    case DG_ERR_NOT_FOUND:  snprintf(buf, cap, "%s", _("固件源不存在"));  break;
    case DG_ERR_BUSY:       snprintf(buf, cap, "%s", _("升级会话被占用")); break;
    case DG_ERR_IO:         snprintf(buf, cap, "%s", _("写入失败"));      break;
    default:                snprintf(buf, cap, "%s %d", _("错误码"), -err); break;
    }
}

/* 状态行 + 按钮可用性(全部状态源汇合后统一渲染) */
static void render_status(const ota_upd_status_t *st)
{
    char t[192];

    /* 升级信息区:有公告才显示版本三元组 */
    if (st->version[0]) {
        snprintf(t, sizeof(t), "%s: %s", _("最新版本"), st->version);
        lv_label_set_text(s_lb_new_ver, t);
        snprintf(t, sizeof(t), "%s: %s", _("发布日期"),
                 st->date[0] ? st->date : "-");
        lv_label_set_text(s_lb_new_date, t);
        lv_label_set_text(s_lb_notes, st->notes[0] ? st->notes : _("无更新说明"));
    }

    switch (st->state) {
    case OTA_UPD_QUERYING:
        lv_label_set_text(s_lb_status, _("查询中..."));
        break;
    case OTA_UPD_AVAILABLE:
        snprintf(t, sizeof(t), "%s %s", _("发现新版本"), st->version);
        lv_obj_set_style_text_color(s_lb_status, DG_COL_OK(), 0);
        lv_label_set_text(s_lb_status, t);
        break;
    case OTA_UPD_DOWNLOADING:
        snprintf(t, sizeof(t), "%s %lu.%lu%%", _("下载中"),
                 (unsigned long)(st->permille / 10),
                 (unsigned long)(st->permille % 10));
        lv_label_set_text(s_lb_status, t);
        break;
    case OTA_UPD_STAGED:
        lv_obj_set_style_text_color(s_lb_status, DG_COL_OK(), 0);
        lv_label_set_text(s_lb_status, _("升级包已就绪,设备将自动升级重启"));
        if (!s_staged_toast_shown) {
            s_staged_toast_shown = true;
            dg_popup_success(_("升级包校验通过,设备将自动升级重启"), 2500,
                             NULL, NULL);
        }
        break;
    case OTA_UPD_FAILED: {
        char why[64];
        fmt_err(why, sizeof(why), st->err);
        snprintf(t, sizeof(t), "%s: %s", _("更新失败"), why);
        lv_obj_set_style_text_color(s_lb_status, DG_COL_ERR(), 0);
        lv_label_set_text(s_lb_status, t);
        break;
    }
    default:
        lv_obj_set_style_text_color(s_lb_status, DG_COL_TEXT(), 0);
        lv_label_set_text(s_lb_status, _("当前已是最新版本"));
        break;
    }

    /* 立即更新:有新版且非自动流程(下载中/已暂存都不可再点) */
    bool apply_ok = (st->state == OTA_UPD_AVAILABLE);
    lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);
    if (apply_ok)
        lv_obj_clear_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);

    /* 检查更新:MQTT 在线才可点 */
    if (s_mqtt_ok)
        lv_obj_remove_state(s_btn_check, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_btn_check, LV_STATE_DISABLED);
}

/* UI 事件泵回调(presenter 转发) */
void page_about_on_result(const ev_ota_update_t *ev)
{
    if (!s_lb_status)
        return;
    ota_upd_status_t st;
    st.state = (ota_upd_state_t)ev->state;
    st.err = ev->err;
    st.permille = ev->permille;
    snprintf(st.version, sizeof(st.version), "%s", ev->version);
    snprintf(st.date, sizeof(st.date), "%s", ev->date);
    snprintf(st.notes, sizeof(st.notes), "%s", ev->notes);
    render_status(&st);
}

void page_about_on_mqtt(bool connected)
{
    s_mqtt_ok = connected;
    if (!s_btn_check)
        return;
    if (connected)
        lv_obj_remove_state(s_btn_check, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_btn_check, LV_STATE_DISABLED);
}

/* ---- 交互 ---- */

static void on_check(lv_event_t *e)
{
    (void)e;
    int rc = ota_update_check();
    if (rc != DG_OK)
        dg_popup_fail(_("检查失败:网络未连接"), 1500, NULL, NULL);
}

static void on_apply(lv_event_t *e)
{
    (void)e;
    int rc = ota_update_apply();
    if (rc == DG_ERR_BUSY)
        dg_popup_fail(_("升级已在进行中"), 1500, NULL, NULL);
    else if (rc != DG_OK)
        dg_popup_fail(_("当前没有可安装的更新"), 1500, NULL, NULL);
}

static void on_auto_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (cfg_set_int("ota_auto_update", on) != DG_OK) {
        if (on)
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        else
            lv_obj_add_state(sw, LV_STATE_CHECKED);   /* 落库失败回弹 */
        dg_popup_fail(_("保存失败"), 1000, NULL, NULL);
    }
}

/* 信息行:左标签(次要色) + 右值(主色),返回值标签供回填 */
static lv_obj_t *info_row(lv_obj_t *parent, const char *label, lv_coord_t y,
                          const char *value)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(lb, DG_COL_TEXT(), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, DG_PAD + 8, y);

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_TEXT(), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -(DG_PAD + 8), y);
    return val;
}

void page_about_create(lv_obj_t *parent)
{
    DG_LOGI("[ABOUT]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    s_mqtt_ok = false;
    s_staged_toast_shown = false;

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("关于设备"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* ---- 设备信息卡 ---- */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, DG_SCREEN_W - 2 * DG_PAD, 240);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_color(card, DG_COL_BG_LIGHT(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);

    const dg_cfg_t *c = cfg_get();
    info_row(card, _("设备名称"), 28, c->device_name);
    s_lb_ver  = info_row(card, _("固件版本"), 92, DG_FW_VERSION);
    s_lb_date = info_row(card, _("构建日期"), 156, DG_BUILD_DATE);

    /* ---- 升级区 ---- */
    lv_coord_t y = 400;
    char t[64];
    s_lb_new_ver = lv_label_create(parent);
    snprintf(t, sizeof(t), "%s: -", _("最新版本"));
    lv_label_set_text(s_lb_new_ver, t);
    lv_obj_set_style_text_font(s_lb_new_ver, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_lb_new_ver, DG_COL_TEXT(), 0);
    lv_obj_align(s_lb_new_ver, LV_ALIGN_TOP_LEFT, DG_PAD + 8, y);

    s_lb_new_date = lv_label_create(parent);
    snprintf(t, sizeof(t), "%s: -", _("发布日期"));
    lv_label_set_text(s_lb_new_date, t);
    lv_obj_set_style_text_font(s_lb_new_date, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_lb_new_date, DG_COL_TEXT(), 0);
    lv_obj_align(s_lb_new_date, LV_ALIGN_TOP_LEFT, DG_PAD + 8, y + 52);

    s_lb_notes = lv_label_create(parent);
    lv_label_set_text(s_lb_notes, "");
    lv_obj_set_style_text_font(s_lb_notes, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_lb_notes, DG_COL_TEXT(), 0);
    lv_label_set_long_mode(s_lb_notes, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lb_notes, DG_SCREEN_W - 2 * (DG_PAD + 8));
    lv_obj_align(s_lb_notes, LV_ALIGN_TOP_LEFT, DG_PAD + 8, y + 108);

    s_lb_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lb_status, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_lb_status, DG_COL_TEXT(), 0);
    lv_obj_align(s_lb_status, LV_ALIGN_TOP_MID, 0, y + 210);

    /* 自动更新行:文字 + 开关 */
    lv_obj_t *auto_lb = lv_label_create(parent);
    lv_label_set_text(auto_lb, _("自动更新"));
    lv_obj_set_style_text_font(auto_lb, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(auto_lb, DG_COL_TEXT(), 0);
    lv_obj_align(auto_lb, LV_ALIGN_TOP_LEFT, DG_PAD + 8, y + 280);

    s_sw_auto = lv_switch_create(parent);
    lv_obj_set_size(s_sw_auto, 100, 56);
    lv_obj_align(s_sw_auto, LV_ALIGN_TOP_RIGHT, -(DG_PAD + 8), y + 272);
    if (cfg_get()->ota_auto_update)
        lv_obj_add_state(s_sw_auto, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_auto, on_auto_switch, LV_EVENT_VALUE_CHANGED,
                        NULL);

    /* ---- 按钮 ---- */
    s_btn_check = dg_btn_create(parent, LV_SYMBOL_REFRESH, _("检查更新"));
    lv_obj_set_size(s_btn_check, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(s_btn_check, LV_ALIGN_TOP_MID, 0, y + 380);
    lv_obj_add_event_cb(s_btn_check, on_check, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_check, LV_STATE_DISABLED);   /* 进页先禁用 */

    s_btn_apply = dg_btn_create(parent, LV_SYMBOL_UPLOAD, _("立即更新"));
    lv_obj_set_size(s_btn_apply, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
    lv_obj_align(s_btn_apply, LV_ALIGN_TOP_MID, 0, y + 380 + DG_BTN_H + DG_PAD);
    lv_obj_add_event_cb(s_btn_apply, on_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);

    /* 初始渲染:状态快照(进页即见上次结果;MQTT 状态等事件) */
    ota_upd_status_t st;
    ota_update_status(&st);
    render_status(&st);
}

void page_about_destroy(void)
{
    s_lb_ver = s_lb_date = NULL;
    s_lb_new_ver = s_lb_new_date = s_lb_notes = s_lb_status = NULL;
    s_btn_check = s_btn_apply = s_sw_auto = NULL;
    DG_LOGI("[ABOUT]", "page destroy");
}
