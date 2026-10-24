/*
 * page_device.c — 设备管理(spec-ui §3.3):语言切换 / NTP 校时 / 网络配置入口
 *
 * 2026-09-27:当前时间与超时行改 1s 实时刷新(配置被 web 等他处修改时
 * 跟随变化,不再显示过期值误导用户);「网络配置」改为进设置页(可编辑)。
 */
#include "cfg.h"
#include "events.h"
#include "dg_log.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"
#include "ui_events.h"

#include <stdio.h>
#include <time.h>

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

static void lang_pick(void *ud, int idx)
{
    (void)ud;
    const char *lang = (idx == 0) ? "zh-CN" : "en-US";
    /* 持久化(cfg_set)+ 立即生效(i18n 切换并发刷新事件,各页重建) */
    if (cfg_set_str("language", lang) == DG_OK)
        i18n_set_language(lang);
    /* i18n 的刷新事件会触发本页重建,无需再刷 */
}

static void on_lang(lv_event_t *e)
{
    (void)e;
    static const char *const opts[] = { "zh-CN", "en-US" };
    dg_popup_choice(_("语言"), opts, 2, lang_pick, NULL, NULL);
}

static void on_ntp(lv_event_t *e)
{
    (void)e;
    /* 真触发一次校正(用配置里的 ntp_server),结果经 EV_NET_NTP_RESULT 回来;
     * 不再弹输入框——服务器地址属设备配置,不该让门禁面板上的人现填 */
    bridge_ntp_sync();               /* C4:出站统一经桥 */
    dg_popup_success(_("NTP 校时中..."), 1000, NULL, NULL);
}

static void on_net(lv_event_t *e)
{
    (void)e;
    navigator_push("net_set");          /* 设置入口:模式/IP/掩码/网关可编辑 */
}

/* 人脸识别阈值(检测/识别/活体滑条,即改即存;2026-09-27 用户需求) */
static void on_face_set(lv_event_t *e)
{
    (void)e;
    navigator_push("face_set");
}

/* 屏幕显示:背光亮度滑条(功耗管理 M1,2026-10-04) */
static void on_display_set(lv_event_t *e)
{
    (void)e;
    navigator_push("display_set");
}

/* ---- 秒数选择项(待机超时/菜单超时;choice 比自由输入防呆,与门禁设置页同款) ---- */

static lv_obj_t *s_lb_standby, *s_lb_menu;   /* 两行按钮上的「当前值」标签 */
static lv_obj_t *s_time;                     /* 当前时间(1s 刷新) */

static void refresh_rows(void *ud)
{
    (void)ud;
    const dg_cfg_t *c = cfg_get();
    char t[64];
    if (s_lb_standby) {
        snprintf(t, sizeof(t), "%s: %ds", _("待机超时"), c->standby_timeout_s);
        lv_label_set_text(s_lb_standby, t);
    }
    if (s_lb_menu) {
        snprintf(t, sizeof(t), "%s: %ds", _("菜单超时"), c->menu_timeout_s);
        lv_label_set_text(s_lb_menu, t);
    }
}

/* 1s 实时刷新:时间 + 各配置当前值(cfg 可被 web/他页修改,跟随变化) */
static void refresh_time(void)
{
    if (!s_time)
        return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    lv_label_set_text(s_time, buf);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_time();
    refresh_rows(NULL);
}

static const int standby_opts[] = { 15, 30, 45, 60 };
static const int menu_opts[] = { 5, 15, 30, 60, 120 };

static void standby_pick(void *ud, int idx)
{
    (void)ud;
    if (cfg_set_int("standby_timeout_s", standby_opts[idx]) == DG_OK)
        dg_popup_success(_("已保存"), 600, refresh_rows, NULL);
    else
        dg_popup_fail(_("保存失败"), 1000, NULL, NULL);
}

static void menu_pick(void *ud, int idx)
{
    (void)ud;
    if (cfg_set_int("menu_timeout_s", menu_opts[idx]) == DG_OK)
        dg_popup_success(_("已保存"), 600, refresh_rows, NULL);
    else
        dg_popup_fail(_("保存失败"), 1000, NULL, NULL);
}

static void on_standby(lv_event_t *e)
{
    (void)e;
    static char buf[4][16];
    const char *opts[4];
    for (int i = 0; i < 4; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%d", standby_opts[i]);
        opts[i] = buf[i];
    }
    dg_popup_choice(_("待机超时"), opts, 4, standby_pick, NULL, NULL);
}

static void on_menu_timeout(lv_event_t *e)
{
    (void)e;
    static char buf[5][16];
    const char *opts[5];
    for (int i = 0; i < 5; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%d", menu_opts[i]);
        opts[i] = buf[i];
    }
    dg_popup_choice(_("菜单超时"), opts, 5, menu_pick, NULL, NULL);
}

/* Web 管理:上位机账号/口令 + 局域网访问地址(spec-network §1) */
static void on_web(lv_event_t *e)
{
    (void)e;
    navigator_push("web_set");
}

/* 关于设备:名称/版本/构建日期 + 检查更新/自动更新(MQTT OTA,2026-10-05) */
static void on_about(lv_event_t *e)
{
    (void)e;
    navigator_push("about");
}

/* 重启设备:红色确认(spec 删除类同规范)→ EV_SYS_REBOOT(sysctl 服务
 * 延迟执行,延迟窗口让本提示先落地) */
static void apply_reboot(void *ud, int idx)
{
    (void)ud;
    if (idx != 0)
        return;
    bridge_reboot(1500);             /* C4:出站统一经桥 */
    dg_popup_success(_("正在重启,设备即将关闭"), 1500, NULL, NULL);
}

static void on_reboot(lv_event_t *e)
{
    (void)e;
    const char *const opts[] = { _("重启") };
    dg_popup_choice_ex(_("确认重启设备?"), opts, 1, 1u << 0, apply_reboot,
                       NULL, NULL);
}

static lv_timer_t *s_timer;

void page_device_create(lv_obj_t *parent)
{
    DG_LOGI("[DEVICE]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("设备管理"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* 左上返回(本页选项即点即存,无保存概念) */
    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* 设备信息(只读):当前时间,1s 刷新(NTP 校时/时区变化即时可见) */
    s_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_time, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_time, DG_COL_TEXT(), 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 90);
    refresh_time();

    /* 表驱动(C6-6:十个同构按钮原先逐个复制粘贴)。label 走运行期 _()
     * 数组(i18n 裸中文扫描要求键字面量出现在 _() 内),动态行 LB_NONE
     * 由 refresh_rows 回填 */
    enum { LB_NONE = 255 };
    static const struct {
        const char *icon;
        uint8_t label;                   /* LBT[] 下标;LB_NONE = 动态文案行 */
        lv_event_cb_t cb;
        bool danger;
    } BTNS[] = {
        { LV_SYMBOL_REFRESH,  0,        on_lang,         false },
        { LV_SYMBOL_UP,       1,        on_ntp,          false },
        { LV_SYMBOL_SETTINGS, 2,        on_face_set,     false },
        { LV_SYMBOL_WIFI,     3,        on_net,          false },
        { LV_SYMBOL_SETTINGS, 4,        on_web,          false },
        { LV_SYMBOL_IMAGE,    5,        on_display_set,  false },
        { LV_SYMBOL_EYE_OPEN, LB_NONE,  on_standby,      false },
        { LV_SYMBOL_SETTINGS, LB_NONE,  on_menu_timeout, false },
        { LV_SYMBOL_LIST,     6,        on_about,        false },
        { LV_SYMBOL_REFRESH,  7,        on_reboot,       true },
    };
    const char *const LBT[] = {
        _("语言"), _("NTP 校时"), _("人脸识别"), _("网络配置"),
        _("Web 管理"), _("屏幕显示"), _("关于设备"), _("重启设备"),
    };
    lv_obj_t *dev_btns[sizeof(BTNS) / sizeof(BTNS[0])] = { 0 };
    for (size_t i = 0; i < sizeof(BTNS) / sizeof(BTNS[0]); i++) {
        const char *label = (BTNS[i].label == LB_NONE) ? "" : LBT[BTNS[i].label];
        lv_obj_t *b = BTNS[i].danger
                          ? dg_btn_create_danger(parent, BTNS[i].icon, label)
                          : dg_btn_create(parent, BTNS[i].icon, label);
        lv_obj_set_size(b, DG_SCREEN_W - 2 * DG_PAD, DG_BTN_H);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 180 + (int32_t)i * (DG_BTN_H + DG_PAD));
        lv_obj_add_event_cb(b, BTNS[i].cb, LV_EVENT_CLICKED, NULL);
        dev_btns[i] = b;
    }

    /* 按钮内追加「当前值」label(与门禁设置页同款手法:btn>row>label) */
    lv_obj_t *standby = dev_btns[6];
    lv_obj_t *menuto = dev_btns[7];
    lv_obj_t *row1 = lv_obj_get_child(standby, 0);
    s_lb_standby = lv_label_create(row1);
    lv_obj_set_style_text_font(s_lb_standby, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_lb_standby, DG_COL_BG(), 0);
    lv_obj_t *row2 = lv_obj_get_child(menuto, 0);
    s_lb_menu = lv_label_create(row2);
    lv_obj_set_style_text_font(s_lb_menu, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(s_lb_menu, DG_COL_BG(), 0);
    refresh_rows(NULL);

    s_timer = lv_timer_create(status_timer_cb, 1000, NULL);
}

void page_device_destroy(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_lb_standby = s_lb_menu = NULL;
    s_time = NULL;
    DG_LOGI("[DEVICE]", "page destroy");
}
