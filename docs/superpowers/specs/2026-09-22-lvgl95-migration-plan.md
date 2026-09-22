# LVGL 9.5 迁移方案 — door-guard(RK3576 K7 门禁)

> 状态:待用户确认后执行。执行环境:WSL 仓库 `/home/olwhistle/doorguard`,交叉工具链与
> `dg-*` 脚本见 `env/env.sh`(先 `source env/env.sh`);板端 root@192.168.2.95(S1550000 串口
> 1500000 8N1;以太网;部署 `dg-deploy`,**部署后必须 md5 核对运行进程**,坑见 DEVLOG)。
> 开工必读:`.agents/skills/door-guard-dev/SKILL.md`(工程纪律)+ 本文。

## 0. 背景与目标

- 现状:LVGL **8.3**(vendored `door-guard/third_party/lvgl`)+ `third_party/lv_drivers`
  (DRM dumb/atomic)+ 自研 evdev 触摸;软渲染预览 27fps(实测定档,无残影,稳定)。
- 为什么迁:①v9 软渲染内置 **aarch64 NEON** 优化路径(fill/blend/memcpy),27fps 上限
  有望突破;②为 **video plane 直通重启**铺路(v9 渲染/失效模型对"透明 UI 层+硬件视频
  plane"支持更干净——8.3 上已完整实现并实测 30fps 零 CPU,但因透明层擦除语义缺失残影
  回退,全套实现见 **git f519b1e**);③8.3 停更,9.x 活跃。
- 目标版本:**LVGL v9.5.0**(2026-02-18 发布)。
- **本方案不做**:Mali GPU draw unit(需自研,另评估)、video plane 重启(迁移完成后
  另立项,基于 f519b1e 适配)。

## 1. 改动面盘点(架构红线:LVGL 只允许存在于 ui 层与 modules/display)

| 范围 | 内容 |
|---|---|
| 替换 | `third_party/lvgl`(8.3→9.5 vendored);弃用 `third_party/lv_drivers`(v9 自带 Linux DRM/FBDEV + EVDEV 驱动) |
| 适配 | `ui/`:10 页面 + 6 自研控件(dg_btn/dg_list/dg_avatar/dg_popup/dg_kbd/dg_preview)+ navigator/bridge/presenters/i18n/ui_events;pages≈2200 行 |
| 适配 | `modules/display/{display_drm,display_sim}.c`(display.h API 保持);`tests/test_widgets、test_i18n` |
| 零改动 | services/全部、modules(camera/sqlite/net…)、proto/、configs/、业务逻辑、事件契约 |

## 2. 关键技术决策

- **D1 版本形态**:9.5.0 vendored 进 `third_party`(与仓内 cjson/mongoose 同法);8.3 在
  git 历史可整链回退(回退点见 §4)。
- **D2 板上显示后端**:优先 v9 内置 Linux DRM 后端(XRGB8888 720×1280)+ v9 EVDEV 触摸
  (fts_ts,Type-B MT)。**预案**:若 v9.5 DRM 后端在 RK3576 有坑(atomic/分辨率/刷新异常),
  回退自研 flush 路径——复用已验证的 atomic 代码(git 3ada4ce 之前版本,~150 行)。
- **D3 颜色格式**:XRGB8888 起步,与现网一致;ARGB/透明属阶段 4(video plane),不在本次。
- **D4 性能**:强制开启 `LV_DRAW_SW_ASM_NEON`(aarch64);以 v8.3 基线(27fps/整机 CPU
  20%)做前后 A/B。
- **D5 兼容策略**:不做 8/9 双栈;迁移在分支进行,主分支始终保持可发布可推板。

## 3. 分阶段执行(每阶段有判据与回退点)

- **阶段 0 入库与编译(0.5 天)**:9.5.0 源码入 `third_party`(8.3 保留为 `third_party/lvgl8`
  备查或直接依赖 git 历史);按 8.3 lv_conf 映射生成 9.5 lv_conf(字体/内存池/NEON/
  颜色深度 32);CMake 切换;v9 SDL sim 编译跑通。
  判据:sim 下 v9 demo 正常渲染。
- **阶段 1 UI 层 API 迁移(1~1.5 天)**:v8→v9 机械改名全量过(lv_img→lv_image、
  lv_disp→lv_display、lv_canvas/lv_timer/lv_style 差异点);逐控件适配,重点:
  dg_popup/dg_kbd(焦点与事件)、dg_avatar(stb 解码→v9 image 描述符)、dg_list、
  dg_btn、dg_preview(相机 XRGB 帧→v9 draw buffer 路径,保持 20ms 泵+seq 去重);
  navigator/presenters 预计微改(v9 定时器/异步 API)。
  判据:sim 零告警编译 + **全页面 sim 走查**(主页/待机/菜单/用户管理/用户编辑/拍摄/
  设备/门禁设置/日志/web 设置)截图留档。
- **阶段 2 板端显示接入(1 天)**:v9 DRM+EVDEV 替换 `display_drm` 板上路径(display.h
  API 不变);七页真机走查 + 触摸(含按下沿唤醒)。
  判据:七页真机截图 + 操作无异常 + **无残影**(direct/局部刷新回归重点)。
  预案:DRM 后端异常 → 自研 flush 回退(D2)。
- **阶段 3 性能与回归(1 天)**:预览 fps、整机 CPU(/proc 采样)、切页响应与 v8.3 基线
  (27fps/20%)对比;NEON 开/关 A/B;test_widgets/test_i18n 迁 v9 后全量 31/31 绿。
  判据:fps ≥ 27(目标 ≥30)、CPU ≤ 基线、测试全绿、交叉编译零告警。
- **回退点**:任一阶段失败 → 回主分支(3ada4ce 及其后,27fps 稳定版),业务交付不受影响。

## 4. 风险与对策

| 风险 | 对策 |
|---|---|
| v8→v9 API 改名遗漏 | 编译器兜底 + 阶段 1 全页面走查清单(九页逐一) |
| 观感差异(v9 主题/字体渲染) | 沿用自研控件与 theme token,主题覆写;走查截图对比 |
| v9 DRM 后端 RK3576 坑 | D2 预案:自研 flush(已验证的 atomic 经验) |
| 触摸 fts_ts MT 适配 | v9 evdev 配置;不行则复用自研 touch_evdev 注册为 v9 indev |
| 性能不及 v8.3 | NEON 强制开;实测仍差 → 停留 8.3(回退),video plane 等 GPU 路线另评估 |
| 残影类回归 | 阶段 2 判据专项:XRGB 不透明架构下 v9 无此问题,但切页/弹窗必须走查 |

## 5. 工时

阶段 0~3 合计 **3.5~4.5 天**(不含真脸对齐标定与 video plane 重启)。

## 6. 迁移后解锁(均另立项)

1. video plane 直通重启(f519b1e 适配 v9,30fps 零 CPU);
2. Mali GPU draw unit 可行性评估;
3. 复杂动效/主题能力。

## 7. 执行纪律提醒(对新会话)

- 遵守 `.agents/skills/door-guard-dev/SKILL.md`:五层架构、透明根页面等渲染决策属
  display/ui 层、测试纪律(每阶段跑 `dg-test`)、DEVLOG 逐条记录;
- 推板前交叉编译零告警;**部署后 md5 核对运行进程**(dg-deploy 写活动槽 + S60 回滚坑);
- 板端卡死时 `pkill -x door-guard`(勿用 -f,会杀掉自己的 ssh)。

## 8. 三对话执行分工(闲时任务)

- 协作协议/文件所有权/API 契约:**docs/superpowers/specs/lvgl9-migration-contract.md**(唯一权威)
- 共享日志:**docs/lvgl9-migration-LOG.md**(开工读、收工写)
- 分工与依赖:C1 基础与显示层(third_party/CMake/modules/display)→ C2 UI 层迁移(ui/**)
  → C3 测试迁移+板端验收+性能回归(tests/、板端、DEVLOG)。串行依赖,前置 `[DONE]` 才开工;
  对应本方案阶段 0-1 / 1-2 / 2-3。
- 回退基线 tag:`lvgl9-baseline`。
