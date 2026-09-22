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
## 2026-09-23 01:35 C2-UI层迁移 [DONE]

- **做了**:
  1. ui/ 全量 API 迁移(机械改名,契约 §3 映射表逐条头文件实测):lv_img_*→lv_image_*
     (create/set_src/antialias/pivot/zoom→scale,256 基准不变)、lv_scr_act→
     lv_screen_active、lv_obj_del→lv_obj_delete、lv_timer_del→lv_timer_delete、
     lv_btn_create→lv_button_create、lv_list_add_btn→lv_list_add_button、
     lv_img_dsc_t→lv_image_dsc_t;涉及 ui.c/navigator/bridge 外全部 pages+widgets。
  2. **v9 image 描述符新语义**(dg_avatar/dg_preview):header.always_zero 取消,
     改设 `header.magic = LV_IMAGE_HEADER_MAGIC`(v9 set_src 靠 magic 识别内存位图,
     不设则识别成文件路径);新增 header.stride(=w*4);cf 用
     LV_COLOR_FORMAT_XRGB8888。20ms 泵/seq 去重/整帧 invalidate 的擦除语义保持原样。
  3. 时基:ui_init 在 display_init **之后** lv_tick_set_cb(dg_ui_tick_ms)
     (v9 DRM 驱动 create 时先装自己的时基,顺序天然正确);port.h 注释同步。
  4. 字体:gen.sh 用 npx 最新 lv_font_conv 重生成 dg_font_cn_16(4359 行);
     产物对 v8/v9 头均语法通过(实测 lv_font_fmt_txt 结构两代兼容);
     lv_conf9 切 LV_FONT_DEFAULT=&dg_font_cn_16 + LV_FONT_CUSTOM_DECLARE。
  5. CMake:解除 C1 的 dg_ui/door-guard 守卫;sim 后端加 DG_SIM_DUMP_AFTER
     (第 N 刷新周期导图,九页走查取证用)。
  6. 九页+弹窗走查(sim,WSLg 实跑):主页/待机/菜单/用户管理/用户编辑/拍摄/
     设备/门禁设置/日志/web 设置 10 图 + 结果弹窗/ID 输入键盘 2 图,存
     `deliverables/lvgl9-c2-walkthrough/`(12 PNG)。走查方法:管理员闸 mock 不可过
     (role 硬编码 NORMAL),沿用上轮「宿主直接渲染同页」验收思路,临时工具
     (/tmp/ui9_walk.c,未入库)+ CMake 临时目标(已撤销)逐页 navigator_switch 导图。
  7. **语义差异清单(v8.3→v9.5,本仓实际遇到)**:
     - image header 结构重排(magic/stride 新增,always_zero 删除);
     - lv_timer_del→lv_timer_delete、lv_obj_del→lv_obj_delete(-ete 后缀);
     - lv_img_set_zoom→lv_image_set_scale(基准同样 256=1x,类型 uint16→uint32);
     - 时基 LV_TICK_CUSTOM(编译期宏)→ lv_tick_set_cb(运行期,有时序要求);
     - theme 默认初始化两代都由 display 创建自动完成,本仓 ui 未显式调用,
       观感一致性靠每控件显式 style(smoke 入口显式调用已用 v9 参数序);
     - image 内存位图必须 magic+stride,否则 set_src 误判;
     - 字体位图格式 lv_font_fmt_txt 两代源码兼容,gen.sh 无需 --lv-version 类参数;
     - 内置 NEON 为 C 内联(arm_neon.h),单 conf 双端必须平台条件编译。
  8. **附带修复(既有 bug,非迁移回归)**:page_logs 分页按钮 prev/next 坐标
     (0/+80)本就重叠,改 -85/+85。
- **门禁**:①v9 下整个 app sim+交叉 clean 编译**零告警**;②12 图走查留档(上表);
  ③字体 v9 工具重生成完成;④dg-test 非 LVGL 用例 **29/29**(widgets/i18n 仍按 C1
  守卫排除,C3 解除);⑤语义差异清单见上;⑥本条目。
- **阻塞**:无。
- **交接(C3 必读)**:
  - **回退通道变化**:ui/ 已 v9-only(方案 D5 不做双栈),`DG_USE_LVGL9=OFF` 从此
    编不过 dg_ui——运行期回退 = `git checkout lvgl9-baseline`(终极回退,契约 §5)。
    OFF 模式下非 UI 库仍可编(C1 已验证),但整 app 不再可用。
  - test_widgets/test_i18n 仍是 v8 API(C3 迁移);解除 tests/CMakeLists.txt 守卫时
    注意 test_touch_evdev 的 v9 分支已链 dg_ui(字体符号)。
  - dg-test 的 build-tests 目录若残留 OFF 缓存会拿 v8 头编 v9 ui 而报错——
    `dg-test -c` 清缓存即可。
  - sim 走查截图里「张三」等**用户数据**字符显示 □□ 为既有行为(字体只收
    lang/*.json 的 UI 字符集),非迁移回归。
  - 板端待验证(C3):v9 DRM atomic 翻转在 RK3576 的实测表现、NEON 实际生效、
    触摸按下沿、性能基线对比(fps/CPU/NEON A/B)。
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
