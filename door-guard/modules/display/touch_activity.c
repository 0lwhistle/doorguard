/*
 * touch_activity.c — 触摸活动监听(显示后端共用的小存储)
 *
 * 板上(touch_evdev)与模拟器(display_sim)的 indev 读回调在「按下沿」
 * 调 display_touch_activity(),转发给 UI 注册的回调(ui.c → bridge_touch →
 * EV_UI_TOUCH)。为什么是按下沿:LVGL 以 ~30Hz 轮询 indev,按下期间每次
 * 采样都转发会把事件总线刷爆。
 */
#include "display.h"

static void (*s_touch_fn)(void);

void display_set_touch_listener(void (*fn)(void))
{
    s_touch_fn = fn;
}

void display_touch_activity(void)
{
    if (s_touch_fn)
        s_touch_fn();
}
