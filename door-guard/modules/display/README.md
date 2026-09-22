# modules/display — 显示与触摸输入模块

同一份 `ui/` 代码跑 PC 模拟器与板上(spec-ui),本模块按端提供后端:

| 文件 | 端 | 说明 |
|---|---|---|
| `display_sim.c` | DG_SIM(宿主) | SDL2 窗口 720×1280,SDL 鼠标 → LVGL pointer |
| `display_drm.c` | 板 | DRM dumb-buffer + atomic(ARGB8888,direct+partial 原位补丁);**不用 /dev/fb0**(B4 固件 mmap EBUSY,实测) |
| video plane | 板 | NV12 dma-buf 直送 VOP2 Overlay(zpos=0 underlay),预览零 CPU;`display_has_video_plane()` 失败自动降级软渲染 |
| `touch_evdev.c` | 板 | evdev 触摸 → LVGL pointer(见下) |

## 触摸(touch_evdev)

- **设备自动探测**:扫 `/dev/input/event*`,名字含 `fts/goodix/gt9/touch` 优先,
  否则首个有 MT 能力(ABS_MT_SLOT)的设备兜底。触摸 IC 随屏组装可能是
  FocalTech(`fts_ts`,2026-09-18 实测当前屏)或 Goodix(GT9xx),DTS 两驱动共存,
  **换屏无需改码**。
- **协议**:Type-B MT slot(ABS_MT_SLOT/TRACKING_ID/POSITION_X/Y)与
  legacy 单点(ABS_X/Y + BTN_TOUCH)都支持;多点时取首个活动槽(LVGL 单点)。
- **坐标映射**:abs 范围 → 屏幕尺寸线性缩放(拿不到范围时恒等映射)。
  触摸轴向与面板装配方向不一致时,启动环境注入校正变量(零魔数,不改码):
  `DG_TOUCH_SWAP_XY=1` / `DG_TOUCH_INVERT_X=1` / `DG_TOUCH_INVERT_Y=1`
  (invert 先作用于原始轴,再做 swap;`/var/log` 日志会打印当前映射)。
- **失败降级**:找不到触摸设备只告警不退出,渲染与 web 上位机路径不受影响。
- **测试**:`tests/test_touch_evdev.c` 宿主直测解析纯函数(`touch_evdev_feed`),
  覆盖 MT 按下/移动/抬起、双槽跟随、legacy、缩放、校准、越界钳制、槽号饱和。

## 使用示例(板端初始化,main 经 ui_init 间接调用)

```c
#include "modules/display/display.h"
lv_init();
if (display_init() != DG_OK) return 1;  /* DRM + 触摸 indev 一次就绪 */
/* 之后主循环周期调 ui_poll()(内部 lv_timer_handler 驱动 flush 与触摸采样) */
```
