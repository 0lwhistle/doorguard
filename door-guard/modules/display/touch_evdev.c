/*
 * touch_evdev.c — 板上触摸输入(evdev → LVGL pointer)
 *
 * 事件解析(touch_evdev_feed)与设备访问(探测/打开/注册)分离:
 * 前者是纯函数可宿主单测,后者只在板构建进 dg_display。
 *
 * 支持:
 *  - Type-B MT slot 协议(fts_ts/goodix 多点):ABS_MT_SLOT/TRACKING_ID/POSITION_*
 *  - legacy 单点协议:ABS_X/ABS_Y + BTN_TOUCH
 *  - abs 范围→屏幕尺寸线性缩放;swap/invert 由 env(DG_TOUCH_*)板级校准
 */
#include "touch_evdev.h"
#include "display.h"
#include "dg_log.h"
#include "lvgl.h"

#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static dg_touch_t s_touch;
static int s_fd = -1;

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);

/* ---------- 纯逻辑:事件解析 ---------- */

static int clamp_idx(int v)
{
    if (v < 0)
        return 0;
    if (v >= DG_TOUCH_MAX_SLOTS)
        return DG_TOUCH_MAX_SLOTS - 1;
    return v;
}

/* raw→屏幕:先线性缩放到 0..out-1,再按需反转;swap 在 feed 末尾统一做 */
static int axis_map(int v, int vmin, int vmax, int out_max, bool invert)
{
    if (out_max <= 0)
        return 0;
    if (vmax <= vmin) /* 范围未探测到:退化为恒等(坐标原样用) */
        return v;
    if (v < vmin)
        v = vmin;
    if (v > vmax)
        v = vmax;
    int r = (int)((int64_t)(v - vmin) * out_max / (vmax - vmin));
    if (r >= out_max)
        r = out_max - 1;
    return invert ? (out_max - 1 - r) : r;
}

static void frame_update(dg_touch_t *t)
{
    if (t->use_mt) {
        t->pressed = false;
        for (int i = 0; i < DG_TOUCH_MAX_SLOTS; i++) {
            if (t->active[i]) {
                t->pressed = true;
                t->leg_x = t->raw_x[i];
                t->leg_y = t->raw_y[i];
                break; /* LVGL pointer 单点:取首个活动槽 */
            }
        }
    } else {
        t->pressed = t->leg_btn;
    }

    /* 缩放到屏幕;触摸面板轴向与装配方向常不一致,env 校正(README) */
    int mx = axis_map(t->leg_x, t->tx_min, t->tx_max, t->scr_w, t->invert_x);
    int my = axis_map(t->leg_y, t->ty_min, t->ty_max, t->scr_h, t->invert_y);
    t->x = t->swap_xy ? my : mx;
    t->y = t->swap_xy ? mx : my;
}

bool touch_evdev_feed(dg_touch_t *t, int type, int code, int value)
{
    switch (type) {
    case EV_ABS:
        if (code == ABS_MT_SLOT) {
            t->use_mt = true;
            t->slot = clamp_idx(value);
        } else if (code == ABS_MT_TRACKING_ID) {
            t->use_mt = true;
            t->active[t->slot] = (value >= 0);
        } else if (code == ABS_MT_POSITION_X) {
            t->raw_x[t->slot] = value;
        } else if (code == ABS_MT_POSITION_Y) {
            t->raw_y[t->slot] = value;
        } else if (code == ABS_X) {
            t->leg_x = value;
        } else if (code == ABS_Y) {
            t->leg_y = value;
        }
        break;
    case EV_KEY:
        if (code == BTN_TOUCH)
            t->leg_btn = (value != 0);
        break;
    case EV_SYN:
        if (code == SYN_REPORT) {
            frame_update(t);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

/* ---------- 设备探测与 LVGL 注册 ---------- */

static bool name_match(const char *name)
{
    if (name[0] == '\0')
        return false;
    char low[128];
    size_t i = 0;
    for (; i < sizeof(low) - 1 && name[i]; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = '\0';
    /* 触摸 IC 随屏组装两家的驱动 DTS 都在,谁探测到谁出现在 /dev/input */
    return strstr(low, "fts") || strstr(low, "goodix") || strstr(low, "gt9") ||
           strstr(low, "touch");
}

static bool has_abs_bit(const unsigned char *bits, int bit)
{
    return bits[bit / 8] & (1u << (bit % 8));
}

static void probe_ranges(dg_touch_t *t)
{
    /* MT 设备取 MT 坐标范围,legacy 设备取 ABS_X/Y 范围(缩放基准);
     * 任一轴拿不到范围时保持 0..0,frame_update 的 axis_map 退化为恒等映射 */
    int ax = t->use_mt ? ABS_MT_POSITION_X : ABS_X;
    int ay = t->use_mt ? ABS_MT_POSITION_Y : ABS_Y;
    struct input_absinfo ai;
    if (ioctl(s_fd, EVIOCGABS(ax), &ai) == 0) {
        t->tx_min = ai.minimum;
        t->tx_max = ai.maximum;
    }
    if (ioctl(s_fd, EVIOCGABS(ay), &ai) == 0) {
        t->ty_min = ai.minimum;
        t->ty_max = ai.maximum;
    }
}

static int open_best_touch(void)
{
    int cand = -1;
    char cand_desc[96] = "";
    char path[32], name[128];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        unsigned char ab[(ABS_MAX + 7) / 8] = { 0 };
        /* EVIOCGBIT 成功返回拷贝字节数(>0),失败为负——不能拿 0 当成功 */
        bool is_mt = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(ab)), ab) >= 0 &&
                     has_abs_bit(ab, ABS_MT_SLOT);
        if (name_match(name)) {
            if (cand >= 0)
                close(cand);
            s_touch.use_mt = is_mt;
            s_fd = fd;
            DG_LOGI("[TOUCH]", "使用 %s(name=%s mt=%d)", path,
                    name[0] ? name : "?", is_mt);
            return 0;
        }
        if (cand < 0 && is_mt) { /* 无匹配名但有 MT 能力:兜底候选 */
            cand = fd;
            snprintf(cand_desc, sizeof(cand_desc), "%s(%s)", path,
                     name[0] ? name : "?");
        } else {
            close(fd);
        }
    }
    if (cand >= 0) {
        s_fd = cand;
        s_touch.use_mt = true;
        DG_LOGI("[TOUCH]", "未匹配到触摸名,使用 MT 兜底设备 %s", cand_desc);
        return 0;
    }
    return DG_ERR_NOT_FOUND;
}

int touch_evdev_start(int scr_w, int scr_h)
{
    memset(&s_touch, 0, sizeof(s_touch));
    s_touch.scr_w = scr_w;
    s_touch.scr_h = scr_h;
    if (open_best_touch() != 0) {
        DG_LOGE("[TOUCH]", "未找到触摸设备(/dev/input/event*)");
        return DG_ERR_NOT_FOUND;
    }
    probe_ranges(&s_touch);

    /* 板级校准(触摸轴向与面板装配不一致时由启动环境注入,零魔数) */
    s_touch.swap_xy = getenv("DG_TOUCH_SWAP_XY") != NULL;
    s_touch.invert_x = getenv("DG_TOUCH_INVERT_X") != NULL;
    s_touch.invert_y = getenv("DG_TOUCH_INVERT_Y") != NULL;
    DG_LOGI("[TOUCH]", "范围 x=%d..%d y=%d..%d → 屏 %dx%d (swap=%d invx=%d invy=%d)",
            s_touch.tx_min, s_touch.tx_max, s_touch.ty_min, s_touch.ty_max,
            scr_w, scr_h, s_touch.swap_xy, s_touch.invert_x, s_touch.invert_y);

    static lv_indev_drv_t drv;
    lv_indev_drv_init(&drv);
    drv.type = LV_INDEV_TYPE_POINTER;
    drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&drv);
    return DG_OK;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (s_fd >= 0) {
        struct input_event ev[32];
        ssize_t n = read(s_fd, ev, sizeof(ev));
        for (ssize_t i = 0; n > 0 && i < n / (ssize_t)sizeof(ev[0]); i++)
            touch_evdev_feed(&s_touch, ev[i].type, ev[i].code, ev[i].value);
    }
    data->point.x = (lv_coord_t)s_touch.x;
    data->point.y = (lv_coord_t)s_touch.y;
    const lv_indev_state_t st =
        s_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    /* 按下沿才通知:LVGL 每拍把 data 清零,上一拍状态要自己记 */
    static bool was_pressed;
    if (st == LV_INDEV_STATE_PRESSED && !was_pressed)
        display_touch_activity();
    was_pressed = (st == LV_INDEV_STATE_PRESSED);
    data->state = st;
}
