# modules/display — 显示与触摸输入模块

同一份 `ui/` 代码跑 PC 模拟器与板上(spec-ui),本模块按端提供后端。
构建开关 `DG_USE_LVGL9` 选择 LVGL 代际(2026-09-23 迁移,契约见
`docs/superpowers/specs/lvgl9-migration-contract.md`),display.h API 两代冻结不变:

| 文件 | 端 | 代际 | 说明 |
|---|---|---|---|
| `display_sim_v9.c` | DG_SIM(宿主) | v9 | SDL2 窗口 720×1280,v9 PARTIAL 渲染逐脏区更新纹理;`DG_SIM_DUMP_BMP=<路径>` 首个刷新周期后导图取证 |
| `display_drm_v9.c` | 板 | v9 | v9 内置 Linux DRM 驱动(dumb buffer×2,DIRECT 双缓冲 + atomic 翻转 + vblank 等待),XRGB8888;薄封装只做设备打开/分辨率核对/触摸注册 |
| `display_sim.c` | DG_SIM(宿主) | v8 | SDL2 窗口 720×1280,SDL 鼠标 → LVGL pointer |
| `display_drm.c` | 板 | v8 | DRM dumb-buffer + legacy modeset,full_refresh 双缓冲页翻转;**不用 /dev/fb0**(B4 固件 mmap EBUSY,实测) |
| `lvgl9_smoke.c` | 两端 | v9 | 最小渲染验证入口(`dg_lvgl9_smoke`,不依赖 ui 层):`DG_SMOKE_FRAMES=<n>` 跑 n 轮退出;验证画面覆盖纯色/圆角/多字号/控件/持续动画 |
| video plane | 板 | 两代 | NV12 dma-buf 直送 VOP2(实验已回退):四桩 `display_has_video_plane/show/hide/clear_fbs` 保持原样,恒不可用;LVGL9 后以 git f519b1e 为基础重启 |
| `touch_evdev.c` | 板 | 两代 | evdev 触摸 → LVGL pointer,`#ifdef DG_USE_LVGL9` 切注册方式(解析/校准完全共用,见下) |
| `touch_activity.c` | 两端 | 无关 | 按下沿通知转发(display.h 冻结契约,与 LVGL 代际无关) |

**为什么 v9 不用内置 evdev/SDL 输入驱动**:display.h 冻结契约要求触摸
「按下沿」通知(`display_touch_activity`,待机唤醒依赖),且板上需要
`DG_TOUCH_*` 轴向校准;v9 内置驱动两者皆无,故沿用自研 read_cb 喂 v9 indev
(契约 §3 允许的回退路径)。

**v9 时基注意**:v9 DRM 驱动在 `lv_linux_drm_create()` 内部会
`lv_tick_set_cb(自带 CLOCK_MONOTONIC 毫秒)`;ui_init 晚于 display_init 执行,
届时以 `dg_ui_tick_ms` 覆盖(C2 落地)。

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
