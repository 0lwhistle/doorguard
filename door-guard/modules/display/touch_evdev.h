/*
 * touch_evdev.h — 板上触摸输入 HAL(evdev → LVGL pointer)
 *
 * 为什么不用 lv_drivers/indev/evdev.c:它只认 ABS_X/ABS_Y 老协议;
 * 板上实测触摸为 fts_ts(Type-B MT slot 协议,DEVLOG 2026-09-18),
 * 且触摸 IC 随屏组装可能是 goodix 或 focaltech(DTS 两驱动共存,谁在谁生效),
 * 故设备按"名字优先 + MT 能力兜底"自动探测。
 *
 * 解析逻辑(touch_evdev_feed)为纯函数,只吃 (type,code,value) 三元组,
 * 不碰内核头文件——tests/test_touch_evdev.c 宿主 gcc 直测。
 */
#ifndef DG_TOUCH_EVDEV_H
#define DG_TOUCH_EVDEV_H

#include "err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MT 槽位上限(ft 常见 5 点;超出饱和处理,门禁场景单指足够) */
#define DG_TOUCH_MAX_SLOTS 10

/** 触摸状态:对外输出 x/y/pressed,其余为解析内部字段(测试可直填直读) */
typedef struct {
    /* 输出:LVGL read_cb 读这里(已完成缩放/旋转映射的屏幕坐标) */
    int x, y;
    bool pressed;

    /* 映射配置:init 时填(屏幕尺寸 + 触摸 abs 范围);env 可覆盖旋转 */
    int scr_w, scr_h;
    int tx_min, tx_max;
    int ty_min, ty_max;
    bool swap_xy, invert_x, invert_y;
    bool use_mt; /**< 探测到 MT slot 协议时置位(决定 SYN 时按哪套取点) */

    /* 内部:MT 槽位跟踪 */
    int slot;
    int raw_x[DG_TOUCH_MAX_SLOTS], raw_y[DG_TOUCH_MAX_SLOTS];
    bool active[DG_TOUCH_MAX_SLOTS];

    /* 内部:legacy 单点协议(ABS_X/Y + BTN_TOUCH) */
    int leg_x, leg_y;
    bool leg_btn;
} dg_touch_t;

/**
 * 探测并打开触摸设备,注册 LVGL pointer indev;失败返回负错误码。
 * scr_w/h 由调用方(显示后端)传入,HAL 不反向依赖 ui/theme。
 * 设备缺位不致命:调用方告警后可继续(web 上位机路径不受影响)。
 */
int touch_evdev_start(int scr_w, int scr_h);

/**
 * 喂一个 evdev 事件(纯逻辑);返回 true 表示该事件是 SYN_REPORT,
 * 一帧结束,x/y/pressed 已更新为最新触点状态。
 */
bool touch_evdev_feed(dg_touch_t *t, int type, int code, int value);

#ifdef __cplusplus
}
#endif

#endif /* DG_TOUCH_EVDEV_H */
