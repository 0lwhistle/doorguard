/*
 * page_net_set.c — 网络配置设置页(接入方式 + IP/掩码/网关)
 *
 * 交互:行点击弹屏幕键盘编辑(预填当前期望值),右上「应用」经总线发给
 * 网络族落地(net_cfg 持久化 + 后台应用);应用是异步的,按钮置「应用中…」,
 * 结果回执弹窗。状态行实时显示实际生效地址(1s 轮询,与配置区分展示——
 * "期望配置"与"实际地址"不一致时用户一眼能看出还没应用/没生效)。
 *
 * 注意:本环境可能没有 DHCP 服务器(纯静态网线直连),切回 DHCP 会拿不到
 * 地址导致失联——选择静态/DHCP 时不立即应用,攒到「应用」一次下发。
 */
#include "page_net_set.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "i18n.h"
#include "modules/net/net_info.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_edit_nav.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>

/* 期望配置草稿(应用前攒在页内;dirty 与 device_config 对比) */
static bool s_is_static;
static char s_ip[16], s_mask[16], s_gw[16];
static bool s_busy;                            /* 应用进行中 */

static lv_obj_t *s_val_mode, *s_val_ip, *s_val_mask, *s_val_gw;
static lv_obj_t *s_addr_line, *s_btn_apply;

/* ---- 点分地址校验(UI 侧初检;最终以 net_cfg_validate 为准) ---- */

static bool parse_ipv4(const char *s)
{
    int a, b, c, d;
    char tail[2];
    if (!s || sscanf(s, "%d.%d.%d.%d%1s", &a, &b, &c, &d, tail) != 4)
        return false;
    return a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
           c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static bool mask_continuous(const char *s)
{
    int a, b, c, d;
    if (!parse_ipv4(s) || sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
        return false;
    uint32_t v = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8) | (uint32_t)d;
    if (v == 0)
        return false;
    return v == 0xFFFFFFFFu || (uint32_t)(v + (v & (~v + 1u))) == 0;
}

static const char *v_ip(const char *t)
{
    if (!parse_ipv4(t) || !strncmp(t, "0.", 2) || !strncmp(t, "127.", 4) ||
        !strncmp(t, "169.254.", 8))
        return _("IP 不合法");
    return NULL;
}

static const char *v_mask(const char *t)
{
    return mask_continuous(t) ? NULL : _("掩码不合法");
}

static const char *v_gw(const char *t)
{
    if (t[0] == '\0')
        return NULL;                           /* 网关可空 = 不设默认路由 */
    if (!parse_ipv4(t) || !strcmp(t, "0.0.0.0"))
        return _("网关不合法");
    return NULL;
}

/* ---- 草稿与 dirty ---- */

static void draft_from_cfg(void)
{
    const dg_cfg_t *c = cfg_get();
    s_is_static = c && strcmp(c->net_mode, "static") == 0;
    snprintf(s_ip, sizeof(s_ip), "%s", c ? c->net_ip : "");
    snprintf(s_mask, sizeof(s_mask), "%s", c ? c->net_mask : "");
    snprintf(s_gw, sizeof(s_gw), "%s", c ? c->net_gw : "");
}

static bool is_dirty(void)
{
    const dg_cfg_t *c = cfg_get();
    if (!c)
        return false;
    if (s_is_static != (strcmp(c->net_mode, "static") == 0))
        return true;
    if (s_is_static)
        return strcmp(s_ip, c->net_ip) != 0 ||
               strcmp(s_mask, c->net_mask) != 0 ||
               strcmp(s_gw, c->net_gw) != 0;
    return false;
}

static void refresh_rows(void)
{
    if (!s_val_mode)
        return;
    dg_btn_set_label(s_val_mode, s_is_static ? _("静态地址") : _("DHCP 自动获取"));
    lv_label_set_text(s_val_ip, s_is_static ? (s_ip[0] ? s_ip : _("无"))
                                            : _("自动获取"));
    lv_label_set_text(s_val_mask, s_is_static ? (s_mask[0] ? s_mask : _("无"))
                                              : _("自动获取"));
    lv_label_set_text(s_val_gw, s_is_static ? (s_gw[0] ? s_gw : _("无"))
                                            : _("自动获取"));
}

/* ---- 行点击:编辑弹窗 ---- */

static void draft_set_static(void *ud, int idx)
{
    (void)ud;
    bool want = (idx == 1);
    if (want == s_is_static)
        return;
    s_is_static = want;
    refresh_rows();
}

static void on_mode(lv_event_t *e)
{
    (void)e;
    const char *const opts[] = { _("DHCP 自动获取"), _("静态地址") };
    dg_popup_choice(_("接入方式"), opts, 2, draft_set_static, NULL, NULL);
}

static void edit_ip(void *ud, const char *text);
static void edit_mask(void *ud, const char *text);
static void edit_gw(void *ud, const char *text);

static void edit_ip(void *ud, const char *text)
{
    (void)ud;
    snprintf(s_ip, sizeof(s_ip), "%s", text);
    refresh_rows();
}

static void on_ip(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = _("IP 地址"), .start_alpha = false, .max_len = 15,
        .initial = s_ip, .validate = v_ip, .on_confirm = edit_ip,
    };
    dg_popup_input(&cfg);
}

static void on_mask(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = _("子网掩码"), .start_alpha = false, .max_len = 15,
        .initial = s_mask, .validate = v_mask,
        .on_confirm = edit_mask,
    };
    dg_popup_input(&cfg);
}

static void edit_mask(void *ud, const char *text)
{
    (void)ud;
    snprintf(s_mask, sizeof(s_mask), "%s", text);
    refresh_rows();
}

static void on_gw(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = _("默认网关"), .start_alpha = false, .max_len = 15,
        .initial = s_gw, .validate = v_gw,
        .on_confirm = edit_gw,
    };
    dg_popup_input(&cfg);
}

static void edit_gw(void *ud, const char *text)
{
    (void)ud;
    snprintf(s_gw, sizeof(s_gw), "%s", text);
    refresh_rows();
}

/* ---- 应用 ---- */

static bool apply_cfg(void)
{
    if (s_busy)
        return false;
    if (s_is_static) {
        if (v_ip(s_ip) || v_mask(s_mask) || v_gw(s_gw)) {
            dg_popup_fail(_("地址不合法,请检查"), 2000, NULL, NULL);
            return false;
        }
    }
    ev_net_cfg_set_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.is_static = s_is_static;
    snprintf(ev.ip, sizeof(ev.ip), "%s", s_ip);
    snprintf(ev.mask, sizeof(ev.mask), "%s", s_mask);
    snprintf(ev.gw, sizeof(ev.gw), "%s", s_gw);
    EVENT_BUS_PUBLISH(EV_NET_CFG_SET, &ev);
    s_busy = true;
    if (s_btn_apply) {
        dg_btn_set_label(s_btn_apply, _("应用中…"));
        lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_DISABLED);
    }
    return true;
}

static void on_apply(lv_event_t *e)
{
    (void)e;
    if (!apply_cfg())
        return;
    dg_popup_success(_("正在应用…"), 1000, NULL, NULL);
}

/* ---- 实际地址状态行(1s 轮询) ---- */

static lv_timer_t *s_addr_timer;

static void addr_timer_cb(lv_timer_t *t)
{
    (void)t;
    net_info_addr_t a;
    net_info_read(&a);
    page_net_set_set_addr(a.ifname, a.ip, a.gw, a.have_ip);
}

void page_net_set_set_addr(const char *ifname, const char *ip,
                           const char *gw, bool have_ip)
{
    if (!s_addr_line)
        return;
    char buf[128];
    if (have_ip)
        snprintf(buf, sizeof(buf), "%s: %s · IP: %s · %s: %s",
                 _("接口"), ifname, ip, _("网关"), gw);
    else
        snprintf(buf, sizeof(buf), "%s", _("当前无网络地址"));
    lv_label_set_text(s_addr_line, buf);
}

void page_net_set_on_result(bool ok, int err, const char *ip)
{
    (void)err;
    s_busy = false;
    if (s_btn_apply) {
        dg_btn_set_label(s_btn_apply, _("应用配置"));
        lv_obj_clear_flag(s_btn_apply, LV_OBJ_FLAG_DISABLED);
    }
    if (ok) {
        draft_from_cfg();                        /* 草稿对齐已保存值 */
        refresh_rows();
        dg_popup_success(_("配置已应用"), 1800, NULL, NULL);
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), _("应用失败(当前 %s)"), ip ? ip : "0.0.0.0");
        dg_popup_fail(buf, 2500, NULL, NULL);
    }
}

/* ---- 装配 ---- */

static lv_obj_t *row_create(lv_obj_t *parent, const char *title,
                            lv_obj_t **val_out, lv_event_cb_t cb)
{
    lv_obj_t *row = dg_btn_create_light(parent, NULL, title);
    lv_obj_set_size(row, DG_SCREEN_W - 2 * DG_PAD, 96);
    *val_out = NULL;
    (void)cb;
    /* 行右侧「当前值」label(dg_btn 结构:btn>row>label,见 page_device 手法) */
    lv_obj_t *inner = lv_obj_get_child(row, 0);
    lv_obj_t *val = lv_label_create(inner);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_BG(), 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -12, 0);
    *val_out = val;
    return row;
}

void page_net_set_create(lv_obj_t *parent)
{
    DG_LOGI("[NETSET]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("网络配置"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* 统一编辑导航:左上返回(带未保存确认);保存退出=发出应用请求 */
    static const dg_edit_nav_ops_t ops = { .is_dirty = is_dirty,
                                           .save = apply_cfg };
    dg_edit_nav_create(parent, &ops, false);

    s_addr_line = lv_label_create(parent);
    lv_obj_set_style_text_font(s_addr_line, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_addr_line, DG_COL_TEXT(), 0);
    lv_obj_align(s_addr_line, LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(s_addr_line, "");

    /* 行:接入方式 / IP / 掩码 / 网关 */
    lv_obj_t *v;
    lv_obj_t *r = row_create(parent, _("接入方式"), &s_val_mode, NULL);
    (void)r;
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_add_event_cb(r, on_mode, LV_EVENT_CLICKED, NULL);

    r = row_create(parent, _("IP 地址"), &s_val_ip, NULL);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 170 + 108);
    lv_obj_add_event_cb(r, on_ip, LV_EVENT_CLICKED, NULL);

    r = row_create(parent, _("子网掩码"), &s_val_mask, NULL);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 170 + 2 * 108);
    lv_obj_add_event_cb(r, on_mask, LV_EVENT_CLICKED, NULL);

    r = row_create(parent, _("网关"), &s_val_gw, NULL);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 170 + 3 * 108);
    lv_obj_add_event_cb(r, on_gw, LV_EVENT_CLICKED, NULL);

    /* 右上「应用配置」 */
    s_btn_apply = dg_btn_create(parent, LV_SYMBOL_OK, _("应用配置"));
    lv_obj_set_size(s_btn_apply, 150, 64);
    lv_obj_align(s_btn_apply, LV_ALIGN_TOP_RIGHT, -DG_PAD, 16);
    lv_obj_add_event_cb(s_btn_apply, on_apply, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, _("静态地址应用后,上位机请改用新地址访问"));
    lv_obj_set_style_text_font(hint, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(hint, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_opa(hint, DG_OPA_TEXT_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    draft_from_cfg();
    refresh_rows();
    s_addr_timer = lv_timer_create(addr_timer_cb, 1000, NULL);
    addr_timer_cb(NULL);
}

void page_net_set_destroy(void)
{
    if (s_addr_timer) {
        lv_timer_del(s_addr_timer);
        s_addr_timer = NULL;
    }
    s_val_mode = s_val_ip = s_val_mask = s_val_gw = NULL;
    s_addr_line = s_btn_apply = NULL;
    s_busy = false;
    DG_LOGI("[NETSET]", "page destroy");
}
