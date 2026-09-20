/*
 * test_touch_evdev.c — 触摸 evdev 纯解析逻辑(宿主 gcc,不碰真实设备)
 *
 * 覆盖:MT Type-B 槽位协议按下/移动/抬起、双槽点跟随、legacy ABS_X/Y 协议、
 * abs 范围→屏幕缩放、swap/invert 校准路径、越界钳制。
 */
#include "modules/display/touch_evdev.h"
#include "dg_test.h"

#include <linux/input.h>
#include <string.h>

static dg_touch_t t;

static bool feed(int type, int code, int value)
{
    return touch_evdev_feed(&t, type, code, value);
}

static void reset(int tx_max, int ty_max)
{
    memset(&t, 0, sizeof(t));
    t.scr_w = 720;
    t.scr_h = 1280;
    t.tx_max = tx_max;
    t.ty_max = ty_max;
}

int main(void)
{
    /* 1. MT Type-B:槽0 按下/移动/抬起 */
    reset(719, 1279);
    feed(EV_ABS, ABS_MT_SLOT, 0);
    feed(EV_ABS, ABS_MT_TRACKING_ID, 5);
    feed(EV_ABS, ABS_MT_POSITION_X, 100);
    feed(EV_ABS, ABS_MT_POSITION_Y, 200);
    DG_CHECK(feed(EV_SYN, SYN_REPORT, 0) == true);
    DG_CHECK(t.pressed);
    DG_CHECK(t.x == 100 && t.y == 200);

    feed(EV_ABS, ABS_MT_POSITION_X, 300);
    feed(EV_ABS, ABS_MT_POSITION_Y, 900);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.pressed && t.x == 300 && t.y == 900);

    feed(EV_ABS, ABS_MT_TRACKING_ID, -1);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(!t.pressed);
    /* 抬起后坐标保留最后位置(LVGL release 事件用) */
    DG_CHECK(t.x == 300 && t.y == 900);

    /* 2. 双槽:点跟随"首个活动槽" */
    reset(719, 1279);
    feed(EV_ABS, ABS_MT_SLOT, 0);
    feed(EV_ABS, ABS_MT_TRACKING_ID, 1);
    feed(EV_ABS, ABS_MT_POSITION_X, 10);
    feed(EV_ABS, ABS_MT_POSITION_Y, 20);
    feed(EV_SYN, SYN_REPORT, 0);
    feed(EV_ABS, ABS_MT_SLOT, 1);
    feed(EV_ABS, ABS_MT_TRACKING_ID, 2);
    feed(EV_ABS, ABS_MT_POSITION_X, 500);
    feed(EV_ABS, ABS_MT_POSITION_Y, 600);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.pressed && t.x == 10 && t.y == 20); /* 槽0 仍活动:取槽0 */
    feed(EV_ABS, ABS_MT_SLOT, 0);
    feed(EV_ABS, ABS_MT_TRACKING_ID, -1);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.pressed && t.x == 500 && t.y == 600); /* 槽0 抬起:跟到槽1 */

    /* 3. legacy 单点协议(ABS_X/Y + BTN_TOUCH) */
    reset(719, 1279);
    feed(EV_ABS, ABS_X, 40);
    feed(EV_ABS, ABS_Y, 80);
    feed(EV_KEY, BTN_TOUCH, 1);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.pressed && t.x == 40 && t.y == 80);
    feed(EV_KEY, BTN_TOUCH, 0);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(!t.pressed);

    /* 4. 范围缩放:0..1023 → 720 宽,0..2047 → 1280 高 */
    reset(1023, 2047);
    feed(EV_ABS, ABS_MT_SLOT, 0);
    feed(EV_ABS, ABS_MT_TRACKING_ID, 7);
    feed(EV_ABS, ABS_MT_POSITION_X, 512);
    feed(EV_ABS, ABS_MT_POSITION_Y, 1024);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.x == 512 * 720 / 1024); /* = 360 */
    DG_CHECK(t.y == 1024 * 1280 / 2048); /* = 640 */

    /* 5. swap_xy / invert_x 校准(env 路径同一字段) */
    reset(719, 1279);
    t.swap_xy = true;
    t.invert_x = true;
    feed(EV_ABS, ABS_X, 100);
    feed(EV_ABS, ABS_Y, 300);
    feed(EV_KEY, BTN_TOUCH, 1);
    feed(EV_SYN, SYN_REPORT, 0);
    /* invert 先作用于原始轴,再 swap:x=触摸Y=300,y=反转后的触摸X=719-100 */
    DG_CHECK(t.x == 300);
    DG_CHECK(t.y == 619);

    /* 6. 越界钳制(触摸值超出 abs 范围不许映射出屏) */
    reset(719, 1279);
    feed(EV_ABS, ABS_X, 5000);
    feed(EV_ABS, ABS_Y, 9000);
    feed(EV_KEY, BTN_TOUCH, 1);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.x == 719 && t.y == 1279);

    /* 7. 槽位号饱和(异常设备喂大槽号不越界写) */
    reset(719, 1279);
    feed(EV_ABS, ABS_MT_SLOT, 42);
    feed(EV_ABS, ABS_MT_TRACKING_ID, 9);
    feed(EV_ABS, ABS_MT_POSITION_X, 5);
    feed(EV_ABS, ABS_MT_POSITION_Y, 6);
    feed(EV_SYN, SYN_REPORT, 0);
    DG_CHECK(t.pressed && t.x == 5 && t.y == 6);

    /* 8. 非触摸事件不构成帧、不改变状态 */
    reset(719, 1279);
    DG_CHECK(feed(EV_MSC, MSC_RAW, 0) == false);
    DG_CHECK(feed(EV_SYN, SYN_MT_REPORT, 0) == false); /* 仅 SYN_REPORT 算帧 */

    DG_TEST_EXIT();
}
