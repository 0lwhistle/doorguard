# Video plane 直通 9.5 重启方案 — door-guard(RK3576 K7 门禁)

> 状态:方案定稿,待用户确认后执行。本方案是 C3 CPU 卡点(2026-09-25)的处置项。
> 执行环境:WSL 仓库 `/home/olwhistle/doorguard`(`source env/env.sh`);板端
> root@192.168.137.130(直插笔记本 ICS;.137.1 是笔记本自身,勿连错);部署必须
> md5 核对运行进程(S60 秒退回滚会切旧包,坑⑤)。
> 开工必读:`.agents/skills/door-guard-dev/SKILL.md`(工程纪律)+
> `docs/lvgl9-migration-LOG.md` 顶部两条 + 本文。

## 0. 背景与目标

- C3 卡点:LVGL9.5 迁移后整机 CPU **28.0%**(稳态,/proc/pid/stat utime+stime 增量),
  超判据「≤基线 20%」(v8 槽同口径实测 20.9%)。C3 因此标 [BLOCKED]。
- 2026-09-25 板上拆分实测:同一 v9 版本仅让取流失败(DG_CAM_DEV 指向不存在节点),
  整机 CPU 降至 **0.3~0.4%**——即 **28% 中约 24~25pp 来自预览软渲染链路**
  (XRGB 帧→lv_image→LVGL 渲染管线重绘进 framebuffer),UI/视觉空转/网络为零头。
- 8.3 时代 video plane 直通(**git f519b1e**,完整实现)实测:预览 30fps 零 CPU,
  整机 20%(含视觉/网络全部业务);因 8.3 透明擦除语义缺失(残影堆积)回退(3ada4ce),
  camera dma-heap 直通池保留在树(无消费方零开销),display 接口桩保留。
- **9.5 已内建透明擦除语义(本方案核心依据,代码实证)**:
  `third_party/lvgl9/src/core/lv_refr.c:1038`——display color_format 带 alpha 时,
  每个刷新周期对每个脏区先 `lv_draw_buf_clear`(清成透明)再画内容。8.3 翻车根因
  已在上游机制层面解决。
- **目标**:预览改走 VOP2 video plane 硬件合成,CPU 回到判据线内(预期显著低于
  20.9%,精确值实测定案),解除 C3 卡点;预览上屏零 CPU。

## 1. 已验证事实(全部为代码/板端实证,非沿用旧 LOG 结论)

| # | 事实 | 出处 |
|---|---|---|
| F1 | 9.5 渲染核心对带 alpha 的 display 逐脏区清透明(擦除语义内建) | `lvgl9/src/core/lv_refr.c:1038` |
| F2 | v9 CPU 28% 构成:预览软渲染 ~25pp,其余 ~3pp | 2026-09-25 板上关取流对比实测 |
| F3 | VOP2:空闲 Overlay 132(Esmart3)/148(Cluster0)支持 NV12+缩放,zpos 0..7 可写,alpha/CSC 齐;**无 90° 硬件旋转**→旋转靠 RGA | DEV_HANDBOOK §2(2026-09-22 硬件探针) |
| F4 | camera dma-heap 直通池(CMA 4 槽,latest_dmabuf/mark_shown 防撕裂)已在树,无消费方零开销 | f519b1e → 3ada4ce 保留,`modules/camera/` |
| F5 | display 侧接口桩:`display_has_video_plane()` 恒 false、show/hide 返 UNSUPPORTED;开关位 DG_UI_PLANE 保留 | `modules/display/display_drm_v9.c` |
| F6 | 9.5 内置 Linux DRM 驱动 fourcc **硬编码 XRGB8888** | `lvgl9/src/drivers/display/drm/lv_linux_drm.c:38` |
| F7 | navigator 页容器默认不透明底、lv_color_mix alpha 真实混合补丁、DG_ERR_UNSUPPORTED 均已保留 | 3ada4ce |
| F8 | 当前显示链路:9.5 内置 DRM 驱动,dumb×2 + DIRECT 双缓冲 + atomic 翻转,XRGB8888,板端 29~30fps 无残影 | C1~C3 已验证 |

## 2. 目标架构

- **UI**:UI plane,ARGB8888,DIRECT 双缓冲(v9 内建脏区透明擦除,F1),screen 透明,
  各页自备不透明底(现口径,F7)——不透明页自然全屏盖住视频层。
- **预览**:rkisp NV12 1280×720 → RGA 旋转 90°(仍 NV12,不转 XRGB)→ dma-heap 池
  → VOP2 video plane(zpos 压 UI 之下)硬件合成+缩放;扫描输出阶段合板,全程 CPU
  零参与(旋转外的所有活都在固定功能硬件)。
- **dg_preview 双模**:直通态=透明洞控件(主页/拍摄页预览区不放像素)+ plane show/hide
  跟随页生命周期;降级态=现软渲染路径(原样保留)。`DG_UI_PLANE=1` 显式开启直通,
  plane 提交失败自动永久降级软渲染(f519b1e 既有机制)。
- **待机语义(决策项,推荐值)**:进待机页时 video plane **hide**,保持现网「黑屏+中央
  时钟」语义(8.3 实验时待机全透明仅剩时钟块、能看到视频,与现网行为不同,不沿用);
  回主页重新 show。

## 3. 改动面

| 范围 | 内容 |
|---|---|
| `third_party/lvgl9` | `drivers/display/drm/lv_linux_drm.c`:fourcc 支持 ARGB8888(编译开关或跟随 display color_format,实现时择简);不改渲染核心 |
| `third_party/lvgl9` | `lv_conf9`:视 ARGB 需要的宏微调(若有) |
| `modules/display` | `display_drm_v9.c`:`DG_UI_PLANE=1` 时 `lv_display_set_color_format(ARGB8888)` + display/screen bg_opa TRANSP;video plane 桩换 f519b1e 实现(plane 发现/atomic 提交,同 fd 禁混 legacy);失败永久降级 |
| `modules/camera` | 激活 dma-heap 直通池(NV12 旋转后写池,latest_dmabuf/mark_shown 槽位协议);直通模式关闭冗余 XRGB 转换 |
| `ui/widgets/dg_preview.c` | 双模:直通态透明洞+plane 生命周期绑定页面;降级态=现状 |
| `ui`(最小) | screen 透明化的 display 级设置(ui_init;页容器不透明底现口径已对) |
| `tools/`(新增) | 把走查工具入库 `tools/board-walk/`(注入库 v3、walk_login/walk_pages/walk_ghost、perf_cpu/perf_switch、raw2png;源码现Windows 侧 `tmp_walk/`) |
| 零改动 | services/ 全部、modules/sqlite、proto/、业务逻辑、configs/(运行时) |

## 4. 分阶段执行(每阶段独立可回退、独立验收)

**阶段 A「透明点亮」**(纯显示层,不碰 video plane,风险隔离)
- A1 lv_linux_drm fourcc ARGB 化;A2 `DG_UI_PLANE=1` 下 display/screen 透明化;
  A3 板上导图取证:不透明页(菜单/子页)与 XRGB 版逐张对拍一致;透明页(主页)
  导图 alpha 分布除控件外为 0(参照 f519b1e「95% 透明洞」验收法);全页无残影。
- 门禁:dg-build 零告警、31/31 绿、全页导图对拍过、fps 不回退(预期仍 29~30)。

**阶段 B「plane 接通」**
- B1 camera 池激活 + plane 发现/提交移植;B2 dg_preview 直通模 + 页面生命周期绑定;
  B3 板上:预览 30fps、切页无残影、待机 hide 语义、按压沿唤醒不受影响、
  plane 提交失败注入→自动降级软渲染。
- 门禁:预览 fps=30;导图无残影;降级路径演练通过。

**阶段 C「全页回归 + 性能定案」**
- C1 七页真机走查(注入库 v3+坐标表,工具已入库 tools/board-walk/);
  C2 残影专项(多轮急速往返→主页终帧);C3 CPU 三方对比
  (v9 软渲染 28.0% / v9 直通 / v8 20.9%)+ fps;判据实测定案。

**阶段 D「收尾」**
- DEV_HANDBOOK §2/§4 硬件结论按实测更新;LOG C3 条目处置(判据过→转 [DONE],
  否则如实);生产恢复(S60+原库+md5);commit+push。

## 5. 风险与预案

| # | 风险 | 预案 |
|---|---|---|
| R1 | 9.5 内置 DRM 驱动 DIRECT+ARGB 的双缓冲 sync 行为未实测 | 阶段 A 先行:最小探针板上验纯色渲染+alpha 读取;若驱动层有坑,预案=自研 flush 回退路(8.3 已验证 atomic 代码,契约 §2 D2 同思路) |
| R2 | ARGB 渲染带宽较 XRGB 变化 | 不透明对象走 fill/blit 快路径,预期同量级;阶段 A 顺带测 fps 确认 |
| R3 | 半透明 scrim/弹窗遮罩在透明底上 | lv_color_mix 真混合补丁已保留(F7);弹窗页不透明底,遮罩合成发生在不透明页;透明主页上的 scrim 阶段 A 实测 |
| R4 | S60 秒退回滚切旧包 | 部署后 md5 三方核对(坑⑤,既有纪律) |
| R5 | 直通态整体失败 | DG_UI_PLANE 开关+失败永久降级软渲染;主线(开关关)零影响 |

## 6. 门禁与转正

- 代码:dg-build 零告警;dg-test 31/31 绿。
- 功能:全页导图对拍无残影;预览 30fps;触摸/按压沿/走查全过。
- 性能:**整机 CPU ≤20%(实测定案)**;fps≥27(目标 30)。
- 文档:DEV_HANDBOOK 实测结论同步;LOG C3 [BLOCKED] 处置(过→[DONE],不过→如实)。
- 回退链:DG_UI_PLANE 关=主线现状;git 历史 f519b1e/3ada4ce 完整可考。
