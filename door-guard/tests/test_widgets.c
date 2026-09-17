/*
 * test_widgets.c — widget 冒烟测试(Phase 5,无头 LVGL)
 *
 * 用 RAM 显示驱动(不计帧)跑真实 widget 代码:每个 widget 创建 + 交互
 * (事件派发)+ 弹窗生命周期,断言对象树与回调,交互过程有日志输出。
 */
#include "dg_test.h"
#include "i18n.h"
#include "lvgl.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_kbd.h"
#include "widgets/dg_list.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define W 720
#define H 1280

static lv_color_t lvbuf[H * 100];
static int s_flush_cnt = 0;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    s_flush_cnt++;
    lv_disp_flush_ready(drv);
    (void)area;
    (void)color_p;
}

static void display_init_headless(void)
{
    static lv_disp_draw_buf_t buf;
    static lv_disp_drv_t drv;
    lv_disp_draw_buf_init(&buf, lvbuf, NULL, W * 100);
    lv_disp_drv_init(&drv);
    drv.hor_res = W;
    drv.ver_res = H;
    drv.draw_buf = &buf;
    drv.flush_cb = flush_cb;
    lv_disp_drv_register(&drv);
}

static void pump(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        lv_timer_handler();
        usleep(5000);       /* 真实流逝时间:自动关闭定时器按 tick 触发 */
    }
}

/* ---- 按钮点击回调计数 ---- */
static int s_btn_clicks = 0;
static void on_btn(lv_event_t *e)
{
    (void)e;
    s_btn_clicks++;
}

/* ---- 键盘回调计数 ---- */
static int s_key_hits = 0, s_bs_hits = 0, s_ok_hits = 0;
static void k_on_key(void *ud, const char *sym)
{
    (void)ud; (void)sym;
    s_key_hits++;
}
static void k_on_bs(void *ud)
{
    (void)ud;
    s_bs_hits++;
}
static void k_on_ok(void *ud)
{
    (void)ud;
    s_ok_hits++;
}

/* ---- 弹窗回调 ---- */
static int s_closed = 0, s_confirmed = 0, s_picked = -1;
static char s_conf_text[32];
static void on_closed(void *ud)
{
    (void)ud;
    s_closed++;
}
static void on_confirm(void *ud, const char *text)
{
    (void)ud;
    s_confirmed++;
    snprintf(s_conf_text, sizeof(s_conf_text), "%s", text);
}

static void on_pick(void *ud, int idx)
{
    (void)ud;
    s_picked = idx;
}

static void click_deep(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++)
        click_deep(lv_obj_get_child(obj, i));
    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
}

int main(void)
{
    lv_init();
    display_init_headless();
    i18n_init(DG_SOURCE_DIR "/ui/lang");

    /* ---- 按钮:创建 + 点击 ---- */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *btn = dg_btn_create(scr, LV_SYMBOL_OK, _("验证成功"));
    DG_CHECK(btn != NULL);
    DG_CHECK(lv_obj_get_child_cnt(btn) >= 1);   /* 图标+文本行存在 */
    lv_obj_add_event_cb(btn, on_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_light = dg_btn_create_light(scr, LV_SYMBOL_SETTINGS, _("设备管理"));
    DG_CHECK(btn_light != NULL);
    lv_obj_add_event_cb(btn_light, on_btn, LV_EVENT_CLICKED, NULL);
    click_deep(scr);
    DG_CHECK(s_btn_clicks == 2);
    printf("[W] dg_btn: create + click OK\n");

    /* ---- 列表:创建 + 行 ---- */
    lv_obj_t *list = dg_list_create(scr);
    DG_CHECK(list != NULL);
    dg_list_add_row(list, LV_SYMBOL_LIST, _("记录查询"), NULL);
    dg_list_add_row(list, LV_SYMBOL_WIFI, _("网络配置"), NULL);
    DG_CHECK(lv_obj_get_child_cnt(list) == 2);
    dg_list_clear(list);
    DG_CHECK(lv_obj_get_child_cnt(list) == 0);
    printf("[W] dg_list: create/add/clear OK\n");

    /* ---- 键盘:数字/退格/OK 回调 ---- */
    dg_kbd_ops_t ops = { .on_key = k_on_key, .on_backspace = k_on_bs,
                         .on_ok = k_on_ok, .user_data = NULL };
    lv_obj_t *kbd = dg_kbd_create(scr, &ops);
    DG_CHECK(kbd != NULL);
    DG_CHECK(lv_obj_get_child_cnt(kbd) == 12);  /* 3x4 */
    click_deep(kbd);
    DG_CHECK(s_key_hits == 10 && s_bs_hits == 1 && s_ok_hits == 1);
    printf("[W] dg_kbd: 12 keys + callbacks OK\n");

    /* ---- 弹窗:成功/失败自动关闭 + 单实例 ---- */
    DG_CHECK(!dg_popup_active());
    dg_popup_success(_("验证成功"), 50, on_closed, NULL);
    DG_CHECK(dg_popup_active());
    pump(200);
    DG_CHECK(!dg_popup_active());               /* 超时自动关 */
    DG_CHECK(s_closed == 1);                    /* 关闭回调触发 */

    dg_popup_fail(_("验证失败"), 50, NULL, NULL);
    dg_popup_success(_("覆盖旧弹窗"), 50, NULL, NULL);  /* 新顶旧 */
    pump(200);
    DG_CHECK(!dg_popup_active());
    printf("[W] dg_popup success/fail: auto close + single instance OK\n");

    /* ---- 输入弹窗:键盘输入 → 确认回调携文本 ---- */
    dg_popup_input(_("请输入密码"), true, on_confirm, NULL, NULL);
    DG_CHECK(dg_popup_active());
    /* 层级:top layer → 遮罩 → 卡片;卡片子对象:标题/textarea/键盘/取消 */
    lv_obj_t *mask = lv_obj_get_child(lv_layer_top(), 0);
    lv_obj_t *card = lv_obj_get_child(mask, 0);
    DG_CHECK(card != NULL);
    /* 找到键盘(含 12 个键位的容器)并点击 "1","2","3" + OK */
    lv_obj_t *kbd_in_popup = NULL;
    uint32_t n = lv_obj_get_child_cnt(card);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(card, i);
        if (lv_obj_get_child_cnt(c) == 12)
            kbd_in_popup = c;
    }
    DG_CHECK(kbd_in_popup != NULL);
    /* 键序:1 2 3 4 5 6 7 8 9 ⌫ 0 OK → 点 1,2,3,OK(键位按钮在 cell 的 child 0) */
    static const uint32_t seq[] = { 0, 1, 2, 11 };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        lv_event_send(lv_obj_get_child(lv_obj_get_child(kbd_in_popup, seq[i]), 0),
                      LV_EVENT_CLICKED, NULL);
    DG_CHECK(s_confirmed == 1);
    DG_CHECK(strcmp(s_conf_text, "123") == 0);
    DG_CHECK(!dg_popup_active());
    printf("[W] dg_popup input: type 123 + confirm OK\n");

    /* ---- 选择弹窗:选项回调携下标 ---- */
    static const char *const opts[] = { "zh-CN", "en-US" };
    dg_popup_choice(_("语言"), opts, 2, on_pick, NULL, NULL);
    lv_obj_t *mask2 = lv_obj_get_child(lv_layer_top(), 0);
    lv_obj_t *card2 = lv_obj_get_child(mask2, 0);
    n = lv_obj_get_child_cnt(card2);
    DG_CHECK(n >= 3);                           /* 标题 + 2 选项 */
    lv_event_send(lv_obj_get_child(card2, 2), LV_EVENT_CLICKED, NULL);
    DG_CHECK(s_picked == 1);
    printf("[W] dg_popup choice: pick idx OK\n");

    /* ---- 刷新泵:渲染流程真跑过(flush 有次数) ---- */
    pump(100);
    DG_CHECK(s_flush_cnt > 0);
    printf("[W] display pump: %d flushes\n", s_flush_cnt);

    DG_TEST_EXIT();
}
