# LVGL9 迁移共享日志(追加写,最新在最上;开工先读本文件最后 50 行)

> 协议见 `docs/superpowers/specs/lvgl9-migration-contract.md` §4。
> 状态标记:`[DONE]` 下一个对话可开工 / `[BLOCKED]` 写明卡点 / `[WIP]` 未完勿接。

---
## (示例格式,勿删)
## 2026-09-22 00:00 C1-基础与显示层 [DONE]
- 做了:…
- 阻塞:…
- 交接:C2 注意 …
---
## 2026-09-23 01:05 C1-基础与显示层 [DONE]

- **做了**:
  1. 入库:用户手动放入的 `third_party/lvgl-9.5.0`(v9.5.0,git 跟踪 1395 文件)按契约
     §2 整体改名 **`third_party/lvgl9`**(git mv,include 根=`#include "lvgl.h"` 不变);
     清理 1398 个 Windows 下载残留 `*:Zone.Identifier` 垃圾文件(.gitignore 已覆盖,不入库)。
  2. `third_party/lvgl9/lv_conf.h`(只写覆盖项,其余走 lv_conf_internal 默认):颜色 32、
     256KB 内建固定池(对齐 8.3)、LV_DEF_REFR_PERIOD 30、OS=NONE、断言同 8.3、
     montserrat 14/20/28/48、SNAPSHOT 关(整屏快照 lv_malloc 超 256KB 池)。
     **NEON 按平台条件开**:`__aarch64__` → LV_DRAW_SW_ASM_NEON(v9.5 NEON 是 C 内联
     arm_neon.h 实现,x86 宿主编不了,单 conf 双端必须条件化);板端实测生效待 C3。
  3. CMake:`DG_USE_LVGL9` option(**默认 ON,契约 §2 定版**;显式 `-DDG_USE_LVGL9=OFF`
     = 完整回退 8.3);`lvgl` 目标名两代复用,上层链接点零改动;v9 时 lvgl 目标 PUBLIC
     注入 libdrm 头路径(v9 的 lvgl.h 在 LV_USE_LINUX_DRM=1 时把 xf86drm 头拉进所有
     使用方,drm.h 在子目录);v9 模式暂不编 `dg_ui`/`door-guard`(ui 仍 v8,C2 解除);
     `tests/CMakeLists.txt` 仅做构建守卫(test_widgets/test_i18n 在 v9 模式排除——契约
     ③允许,test_touch_evdev 条件化头路径/编译定义,**测试内容零改动**)。
  4. modules/display:`display_drm_v9.c`(v9 内置 Linux DRM:dumb×2+DIRECT 双缓冲+
     atomic 翻转+vblank 等待,与 8.3 已验证 full_refresh 页翻转架构等价)、
     `display_sim_v9.c`(自写 SDL+PARTIAL,`DG_SIM_DUMP_BMP` 首周期导图取证)、
     `touch_evdev.c` 双栈化(#ifdef 切注册,解析/校准/按下沿全共用)、
     `lvgl9_smoke.c`(最小渲染入口,不依赖 ui 层);video plane 四桩原样;README 更新。
     `third_party/lv_drivers/DEPRECATED.md` 标记弃用(OFF 仍编,ON 不编)。
- **门禁**:①ON:sim+交叉 clean 重建**零告警**,smoke 300 帧通过+像素采样正确
  (bg #1b2a41/白卡/蓝按钮,WSLg 实跑);②OFF:交叉+sim 全量零告警,产物齐全;
  ③dg-test:v9 模式 **29/29**(widgets/i18n 按契约排除,记入本条),v8 模式 **31/31**;
  ④`display.h` git diff = 0;⑤本条目。
- **阻塞**:无(板端实跑按纪律归 C3;交叉 smoke 产物已就绪,板端 DRM/EVDEV 验证在 C3)。
- **交接(C2 必读)**:
  - v9 模式下 `dg_ui`/`door-guard` 目标被 `if(NOT DG_USE_LVGL9)` 守卫,**迁完 ui 后解除**
    (CMakeLists 两处);tests 守卫归 C3 解除。
  - `lv_conf9` 字体:C2 重生成 `dg_font_cn_16` 后改 `LV_FONT_DEFAULT &dg_font_cn_16` +
    `LV_FONT_CUSTOM_DECLARE`(现暂 montserrat_14,注释已标)。
  - **时基**:v9 DRM 驱动 create 时自带 lv_tick_set_cb(CLOCK_MONOTONIC);ui_init 晚于
    display_init,C2 在 ui/port.c 调 lv_tick_set_cb(dg_ui_tick_ms) 天然覆盖,勿提前。
  - sim 头显跑法:`dg-build-pc -r`(WSLg 可用);无头自动化:`SDL_VIDEODRIVER=dummy`
    (软件渲染自动回退)+ `DG_SMOKE_FRAMES`/`DG_SIM_DUMP_BMP`。
  - 已验证 API 映射(头文件实测):lv_indev_create()+set_type/set_read_cb/set_display、
    lv_display_create/set_buffers/set_flush_cb/flush_ready/flush_is_last、
    lv_tick_set_cb、lv_theme_default_init(disp,primary,secondary,dark,font)、
    lv_screen_active、lv_button_create、lv_spinner_create(parent)。
---
