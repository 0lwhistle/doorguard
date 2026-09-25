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
## 2026-09-25 17:20 C3 处置立项:video plane 直通 9.5 重启(方案定稿,CPU 卡点出路)

- **实测补强(2026-09-25 晚,修正本日上条目的归因深度)**:板上关取流对比——同一
  v9 版本整机 CPU 从 28.0% 降至 0.3~0.4%,即 28% 中约 24~25pp 来自预览软渲染
  (XRGB→lv_image→LVGL 渲染管线重绘),UI/视觉空转/网络仅零头。CPU 回归不是
  LVGL 9 渲染整体变慢,而是预览上屏一条链——GPU/UI 绘制优化均打不中靶心。
- **9.5 透明擦除语义实证(方案核心依据)**:lvgl9/src/core/lv_refr.c:1038——
  display color_format 带 alpha 时,每刷新周期逐脏区 lv_draw_buf_clear(清透明)
  再画内容。8.3 翻车根因(透明区域不写 fb→残影堆积)已在上游机制层面解决。
  遗留:内置 DRM 驱动 fourcc 硬编码 XRGB8888(lv_linux_drm.c:38)需 ARGB 化。
- **方案定稿**:docs/superpowers/specs/2026-09-25-video-plane-v9-restart-plan.md
  (目标架构/改动面/四阶段门禁/风险预案/待机 hide 决策)。要点:DG_UI_PLANE=1
  显式开关+失败永久降级软渲染,主线零影响;camera dma-heap 池(F4)与 display
  接口桩(F5)已在树;走查工具入库 tools/board-walk/ 一并纳入本任务。
- **状态**:C3 维持 [BLOCKED],待本方案实施后按 §6 实测定案(CPU≤20% 且无残影
  →转 [DONE])。下一对话按方案文档直接开工。
---
## 2026-09-25 15:45 C3-测试与板端验收 [BLOCKED](唯一卡点:整机 CPU 超基线判据;其余门禁全过)

- **做了**:
  1. 走查环境重建:注入库重写三代(v1 覆盖写竞态丢命令→v2 追加写+库内动作队列→
     v3 独立线程发射+事件帧瘦身 P4/R2;gesture 关闭下 SLOT/BTN 冗余可去)。实测
     弹窗+预览软渲染负载下事件消费 ~110ms/事件、主页 ~30ms/事件;P/R 跨帧间隔
     由「读空+80ms」保证,PRESSED 时长 ~0.5s(<400ms 长按阈值会吞 CLICKED)。
  2. 登录链路真机走通:菜单按钮→管理员认证(5s 内点验证)→UID→方式选择→密码→
     「管理员验证通过进菜单」。FSM 时序实测:UID/密码数字键不重置 5s 计时(确认
     才提交),弹窗打开即计时;~660ms/键 ×5 键=3.3s 可过窗。验证按钮(非菜单入口)
     验证成功只回普通模式不进菜单(FSM succeed/back_to_normal),进菜单必走
     verify_from_admin 路径。
  3. **发现并修复 v9 迁移真语义差异(责任阶段 C2,C3 发现)**:page_menu 宫格卡片
     内容容器(lv_obj col)默认 CLICKABLE 赢过卡片吞 CLICKED——dg_btn 行容器同款
     (见 09-23 条目),menu 自绘未复用 dg_btn 故漏修;grid 布局容器顺手 clear。
     两处 lv_obj_clear_flag(CLICKABLE),dg-build 零告警,修复后宫格触摸全通。
  4. **七页真机走查全过(门禁③✓)**:菜单/用户管理/用户编辑/门禁设置/记录查询
     (30 条真实业务日志)/设备管理/web 设置逐页导图,中文渲染/布局/无残影;
     残影专项=多轮急速往返后主页终帧干净✓。归档
     deliverables/lvgl9-c3-board-walkthrough/50~58(58=记录查询「最后一行底色
     发灰」观察项:仅导航漂移+无效点击序列后出现一次,正常路径未复现,留证)。
  5. 性能报告(门禁④):
     - fps:29~30(基线 27,目标 30)✓
     - 切页响应:tap→帧变化 50ms(菜单→用户管理)/79ms(返回),注入+md5 轮询法
       (粒度 ~70ms);v8 无同口径基线,绝对值如实
     - NEON A/B(/proc/pid/stat utime+stime/10s,稳态,无注入):NONE 37.1% vs
       NEON 28.0% 单核,**NEON 相对降 25%,生效 ✓**
     - **v8/v9 同口径对比(A 槽 v8 实测 20.9%):v9 28.0%,回归 +7.1pp(+34% 相对)**
  6. 生产恢复 ✓:原库(db/wal/shm/key)自 /root/dg_db_backup_v8/ 还原,S60 启动,
     md5 三方一致(3ce8bcab),fps=30,生产库 001/002 确认。
- **卡点(门禁④ CPU 判据不过)**:判据「CPU≤基线 20%」,实测 v9 NEON 28.0% vs
  v8 20.9%(同法同状态:主页+预览流,稳态)。绝对值 0.28 核(四核整机 7%)产品
  不阻塞,但迁移回归真实存在;fps/走查/NEON 有效性均达标。
- **交接(建议)**:
  - CPU 回归归因线索:同预览软渲染(20ms 泵+整帧 invalidate)同 30ms 刷新,v9
    渲染管线开销更高,NEON 已生效;方向=video plane 直通重做(f519b1e 可考)、
    渲染管线专项,或用户裁决接受偏差后本条转 [DONE]。
  - 注入库/走查/性能脚本源码在 Windows 侧 tmp_walk/(dg_touch_inject.c v3、
    walk_login/walk_pages/walk_ghost、perf_cpu/perf_switch、raw2png),建议入库
    tools/board-walk/(本次未动:C3 文件边界只许 tests/docs/板端操作)。
  - 板端:B 槽=v9 NEON 最终版(md5 3ce8bcab)=生产运行中(S60);A 槽=v8;
    辅助物留板 /tmp(dg_touch_inject.so、walk*.sh、perf_*.sh)+WSL /root/dg_walk/。
- **门禁记分**:①零告警✓ ②31/31✓ ③七页走查+无残影+触摸✓ ④fps✓/**CPU✗**/
  NEON✓ ⑤DEVLOG/PROJECT_PLAN✓ ⑥commit+push✓(本条目随本 commit)
---
## 2026-09-23 03:30 C3-测试与板端验收 [WIP](测试迁移+部署+登录流已过;子页走查与性能报告未完)

- **做了**:
  1. 测试迁移:`test_widgets` 迁 v9(RAM 显示驱动换 lv_display_create/set_buffers
     PARTIAL、lv_tick_set_cb 注入真实时基、lv_event_send→**lv_obj_send_event**
     (v9 公开 API 变了,内部 lv_event_send 签名不兼容));`test_i18n` 仅头路径按代际
     切换(lv_font_get_glyph_dsc 两代同签名)。tests 守卫解除,dg-test **31/31 全绿
     零告警**(契约门禁② ✓),交叉编译零告警(① ✓)。
  2. 板端部署(唯一上板者,md5 三方核对):首部即崩 **free(): invalid pointer**
     ——display_drm_v9 用 libc free 释放驱动 lv_zalloc(tlsf 池)分配的设备路径
     → 改 lv_free(宿主 sim 不编此文件故 C1 未暴露;**责任阶段 C1**,C3 发现并修复)。
     崩溃循环曾把 dropbear 拖挂(板上 D 状态进程,pidof 都阻塞)→ `reboot -f` 恢复;
     **S60 三次秒退自动回滚到 v8 槽实战生效**(回退通道验证 ✓)。修复版入非活动槽 B
     (v8 保留在 A 作回退位),符号链接切 B:DRM atomic 后端就绪、fts_ts 触摸注册、
     预览 fps=27~30(cost 1~2ms)。
  3. 触摸走查方法:LD_PRELOAD 注入库(/tmp/dg_touch_inject.c,钩 read 合成 Type-B
     MT 事件流,**走真实 evdev 解析/校准/按下沿链路**,非 lv_event 直灌)。验证:
     **按下沿唤醒 ✓**(待机→主页)、**完整管理员登录流 ✓**(验证→ID 10001→密码
     123456→成功弹窗「验证成功 张三」,板上 DB 种子账号)。
  4. **发现并修复 v9 迁移真语义差异(重要)**:dg_btn 的内容行容器在 v9 命中测试中
     胜出吞掉 CLICKED(行内 label v9 默认不可点击,但行容器 lv_obj 默认 CLICKABLE
     且是搜索终点)。v8 时代 label 同样吞点击但行仅 71×29、真人/注入难命中;注入恒打
     按钮中心必现。修复=两处行容器 `lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE)`
     (dg_btn.c)。诊断链:LV_USE_LOG(已关)+ vendored lv_indev 临时 printf(已
     git 还原)拿铁证 `pressed at x:604 y:1216` → `hit obj=(568,1201 71x29)`。
  5. 无残影专项(部分):多轮 home↔standby 切换后导图干净,无上一页残留;
     板端取证 14 张存 `deliverables/lvgl9-c3-board-walkthrough/`(主页/唤醒/UID
     弹窗/方式选择/成功弹窗/待机/回主页等)。
  6. display_drm_v9 补走查取证:DG_WALK_DUMP_DIR 设置时,触摸 /tmp/dg_shot 即把
     活动缓冲导 RAW(v8 时代 DG_DUMP_FIRST_FRAME 的对等物;C1 裁掉了,本阶段补,
     **责任阶段 C1**;env 门控,生产路径一次 getenv 零开销)。
- **未完(C3 门禁③④⑤未过,不许标 DONE)**:
  ① **七页真机走查只走了主页/待机/弹窗流**;菜单子页的触摸进入未走通(网格坐标
  未命中 + 菜单 15s 无操作自动回主页)——dg_btn 修复后网格按钮理论可点,但本轮
  预算用尽;子页渲染正确性目前仅有 sim 证据(C2 十二图)。
  ② **性能报告未做**:fps 侧有日志证据(v9 27~30 vs v8 基线 27,已达标);整机
  CPU /proc 采样、切页响应、**NEON 开/关 A/B**(lv_conf9 `LV_USE_DRAW_SW_ASM`,
  关=改 NONE 重编)未测。
  ③ 文档:DEVLOG/PROJECT_PLAN 已随本条更新;走查补完与性能报告后 C3 才可 [DONE]。
- **板端现况(交接必读)**:B 槽=v9 最终修复版(md5 ec207149396da610fd6fb0642cd848c6,
  含 dg_btn 修复+取证导图;诊断补丁与 LV_USE_LOG 已全部还原),A 槽=v8 回退位;
  **原生产 DB 已还原**(走查种子库的备份在板上 /root/dg_db_backup_v8/),S60 生产
  方式运行,fps=29;走查辅助物(/tmp/dg_touch_inject.so、/tmp/walk*.sh)留板 /tmp
  便于续做;种子账号(10001/张三/管理员/密码 123456)在种子库里,现用原库无此号,
  再走查需重播种(sim/data/door-guard.db 已 checkpoint 含 10001)。
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
