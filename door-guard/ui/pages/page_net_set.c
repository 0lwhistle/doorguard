/*
 * page_net_set.c — 网络配置设置页(接入方式 + IP/掩码/网关)
 *
 * 交互:行点击弹屏幕键盘编辑(预填当前期望值),右上「应用」经总线发给
 * 网络族落地(net_cfg 持久化 + 后台应用);应用是异步的,按钮置「应用中...」,
 * 结果回执弹窗。
 *
 * 注意:本环境可能没有 DHCP 服务器(纯静态网线直连),切回 DHCP 会拿不到
 * 地址导致失联——选择静态/DHCP 时不立即应用,攒到「应用」一次下发。
 */
#include "page_net_set.h"
#include "cfg.h"
#include "dg_log.h"
#include "events.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "theme.h"
#include "valid.h"
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
static lv_obj_t *s_btn_apply;

/* ---- 点分地址校验(UI 侧初检;最终以 net_cfg_validate 为准)----
 * 形态规则在 proto/valid(dg_valid_ipv4,一处定义);对象段取值(IP 不得为
 * 0/环回/链路本地,掩码连续,网关可空非 0)是网络语义,本页判 */

static bool quad_vals(const char *s, unsigned v[4])
{
    return dg_valid_ipv4(s) == DG_OK &&
           sscanf(s, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]) == 4;
}

static bool mask_continuous(const char *s)
{
    unsigned v[4];
    if (!quad_vals(s, v))
        return false;
    uint32_t m = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    /* 连续性:高 1 低 0 ⟺ m 加最低置位位回绕到 0(net_cfg_mask_plen 同款) */
    return m != 0 && (uint32_t)(m + (m & (~m + 1u))) == 0;
}

static const char *v_ip(const char *t)
{
    unsigned v[4];
    if (!quad_vals(t, v) || v[0] == 0 || v[0] == 127 ||
        (v[0] == 169 && v[1] == 254))
        return _("IP 不合法");                   /* 0.x/环回/链路本地无意义 */
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
    unsigned v[4];
    if (!quad_vals(t, v) || (v[0] | v[1] | v[2] | v[3]) == 0)
        return _("网关不合法");
    return NULL;
}

/* ---- 点号固定输入(键盘无点号键,用户只敲数字)----
 * 预填=现值补零成 12 位;键入中 dg_ipv4_autodot 每 3 位自动插点号;
 * 确认门禁=恰好 12 位,存盘前规范化去前导零(否则 inet_pton 类解析拒收) */

static bool input_complete(const char *t)
{
    int n = 0;
    for (const char *p = t; *p; p++)
        n += (*p >= '0' && *p <= '9');
    return n == 12;
}

static const char *v_ip_input(const char *t)
{
    if (!input_complete(t))
        return _("请输满 12 位数字,点号自动补全");
    return v_ip(t);
}

static const char *v_mask_input(const char *t)
{
    if (!input_complete(t))
        return _("请输满 12 位数字,点号自动补全");
    return v_mask(t);
}

static const char *v_gw_input(const char *t)
{
    if (t[0] == '\0')
        return NULL;                           /* 清空 = 不设网关 */
    if (!input_complete(t))
        return _("请输满 12 位数字,点号自动补全");
    return v_gw(t);
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
    lv_label_set_text(s_val_mode, s_is_static ? _("静态地址") : _("DHCP 自动获取"));
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

static void edit_mask(void *ud, const char *text);
static void edit_gw(void *ud, const char *text);

static void edit_ip(void *ud, const char *text)
{
    (void)ud;
    char norm[16];
    if (!dg_ipv4_normalize(text, norm, sizeof(norm)))
        snprintf(norm, sizeof(norm), "%s", text);  /* 校验已拦,防御保底 */
    snprintf(s_ip, sizeof(s_ip), "%s", norm);
    refresh_rows();
}

static void on_ip(lv_event_t *e)
{
    (void)e;
    char init[16];
    dg_ipv4_pad(s_ip, init, sizeof(init));     /* 现值 → 12 位补零(点号固定) */
    const dg_popup_input_cfg_t cfg = {
        .title = _("IP 地址"), .start_alpha = false, .max_len = 15,
        .initial = init, .format = dg_ipv4_autodot,
        .validate = v_ip_input, .on_confirm = edit_ip,
    };
    dg_popup_input(&cfg);
}

static void on_mask(lv_event_t *e)
{
    (void)e;
    char init[16];
    dg_ipv4_pad(s_mask, init, sizeof(init));
    const dg_popup_input_cfg_t cfg = {
        .title = _("子网掩码"), .start_alpha = false, .max_len = 15,
        .initial = init, .format = dg_ipv4_autodot,
        .validate = v_mask_input, .on_confirm = edit_mask,
    };
    dg_popup_input(&cfg);
}

static void edit_mask(void *ud, const char *text)
{
    (void)ud;
    char norm[16];
    if (!dg_ipv4_normalize(text, norm, sizeof(norm)))
        snprintf(norm, sizeof(norm), "%s", text);
    snprintf(s_mask, sizeof(s_mask), "%s", norm);
    refresh_rows();
}

static void on_gw(lv_event_t *e)
{
    (void)e;
    char init[16];
    dg_ipv4_pad(s_gw, init, sizeof(init));     /* 空 = 清网关语义保持 */
    const dg_popup_input_cfg_t cfg = {
        .title = _("默认网关"), .start_alpha = false, .max_len = 15,
        .initial = init, .format = dg_ipv4_autodot,
        .validate = v_gw_input, .on_confirm = edit_gw,
    };
    dg_popup_input(&cfg);
}

static void edit_gw(void *ud, const char *text)
{
    (void)ud;
    char norm[16];
    if (!dg_ipv4_normalize(text, norm, sizeof(norm)))
        snprintf(norm, sizeof(norm), "%s", text);  /* 空/异常原样存 */
    snprintf(s_gw, sizeof(s_gw), "%s", norm);
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
    bridge_net_cfg_set(s_is_static, s_ip, s_mask, s_gw);   /* C4:出站统一经桥 */
    s_busy = true;
    if (s_btn_apply) {
        dg_btn_set_label(s_btn_apply, _("应用中..."));
        lv_obj_add_state(s_btn_apply, LV_STATE_DISABLED);
    }
    return true;
}

static void on_apply(lv_event_t *e)
{
    (void)e;
    if (!apply_cfg())
        return;
    dg_popup_success(_("正在应用..."), 1000, NULL, NULL);
}

void page_net_set_on_result(bool ok, int err, const char *ip)
{
    (void)err;
    s_busy = false;
    if (s_btn_apply) {
        dg_btn_set_label(s_btn_apply, _("应用配置"));
        lv_obj_remove_state(s_btn_apply, LV_STATE_DISABLED);
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

/* 行:浅底按钮,标示左对齐,右侧「当前值」黑字(按下蓝底时随标题转白) */
static lv_obj_t *row_create(lv_obj_t *parent, const char *title,
                            lv_obj_t **val_out)
{
    lv_obj_t *row = dg_btn_create_light(parent, NULL, title);
    lv_obj_set_size(row, DG_SCREEN_W - 2 * DG_PAD, 96);
    *val_out = NULL;
    /* 标示左对齐(2026-09-30 用户要求):dg_btn 的内容行默认整行居中,改靠左 */
    lv_obj_t *inner = lv_obj_get_child(row, 0);
    lv_obj_align(inner, LV_ALIGN_LEFT_MID, DG_PAD, 0);
    /* 值必须挂在 btn 上:内容行是 flex 容器,子对象会被流式布局摆到标题旁,
     * lv_obj_align 被无视;作为 btn 的直接子对象才能右贴边 */
    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_color(val, DG_COL_BG(), LV_STATE_PRESSED);
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

    /* 行:接入方式 / IP / 掩码 / 网关(状态行已移除,行体上移补位) */
    lv_obj_t *r = row_create(parent, _("接入方式"), &s_val_mode);
    (void)r;
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_add_event_cb(r, on_mode, LV_EVENT_CLICKED, NULL);

    r = row_create(parent, _("IP 地址"), &s_val_ip);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 110 + 108);
    lv_obj_add_event_cb(r, on_ip, LV_EVENT_CLICKED, NULL);

    r = row_create(parent, _("子网掩码"), &s_val_mask);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 110 + 2 * 108);
    lv_obj_add_event_cb(r, on_mask, LV_EVENT_CLICKED, NULL);

    r = row_create(parent, _("网关"), &s_val_gw);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 110 + 3 * 108);
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
}

void page_net_set_destroy(void)
{
    s_val_mode = s_val_ip = s_val_mask = s_val_gw = NULL;
    s_btn_apply = NULL;
    s_busy = false;
    DG_LOGI("[NETSET]", "page destroy");
}
