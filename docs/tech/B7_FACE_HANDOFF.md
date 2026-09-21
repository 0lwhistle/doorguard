# B7 人脸识别 — 交接文档(2026-09-18,上下文压缩交接)

> ⚠️ **2026-09-21 路线变更:本文的 ROCKIVA 方案已降为备选,主线改为自组 rknn
> (RetinaFace + ArcFace)。** 新接手请先读 `door-guard/services/vision/README.md`
> (三后端与 rknn 链路)与 `door-guard/models/README.md`(模型清单、重转步骤、板上实测)。
> 本文仍有价值的部分:§0.1 数据流/模式状态机/线程模型、§0.2 holder 装配、
> §3 的 API 与踩坑备忘(其中 ROCKIVA 专有项仅备选路线适用)。
> 变更原因见 §2.4 末尾的补充。

> 读本文前先读 `docs/DEVLOG.md` 顶部两条(B6 相机/OTA 已完成)。
> 本文是 B7 的唯一交接事实源:已完成/待做/坑/API 备忘全在此。
> **2026-09-18 晚更新**:§2 的 ①②③⑤(S60 env)⑦⑧⑨ 已完成并上板;
> 板上 `vision_backend` 因缺人脸模型 = ERROR(降级正常),模型拷入后即可联调。

## 0. 一句话状态

B7 代码全部完成(holder 接入 / mode 联动 / 活体留口 / 相机 NV12 出口 /
特征库同步),**已推板**:14 模块 holder 注册表全 READY(除 vision_backend
因缺人脸模型 ERROR),相机/UI/web/触摸正常。**只剩**:用户从 VM 拷人脸模型
→ 联调(录入人脸 / 1:N 命中 / 特征长度确认)。

## 0.1 技术架构(定稿)

### 分层与数据流

```
                      ┌──────────────────────────────────────────┐
                      │        camera_board(B6 已有,已加 NV12 出口)│
 IMX415 → ISP mainpath │  DQBUF → RGA 转 XRGB → camera_latest → UI 预览│
 NV12 1280×720 ──────→ │  DQBUF → on_frame 零拷贝包帧 → ROCKIVA_PushFrame│
                      │        ↑ on_release(frameId) → 延迟 QBUF     │
                      └───────────────┬──────────────────────────┘
                                      │(camera 轮询线程,PushFrame 异步不阻塞)
   ┌──────────────────────────────────▼───────────────────────────────┐
   │ vision_rockiva(ROCKIVA 内部线程做 NPU 推理,回调出结果)             │
   │  detCallback → 最大脸万分比框 → 旋转映射(720×1280)→ EV_VISION_FACE_BOX│
   │  analyseCallback → 特征(qualityOK)→                                │
   │    ├─ mode=DETECT_1N:SearchFeature("dg_users") ≥阈值                │
   │    │    且 db_user_get 存在 → EV_VISION_MATCH_1N ──┐                │
   │    ├─ mode=VERIFY_11:FeatureCompare vs 目标特征     │               │
   │    │    → EV_VISION_VERIFY_11 ────────────────────┤                │
   │    └─ 恒定:缓存最新特征(3s 新鲜窗,录入 CAPTURE_REQ 取用)│            │
   └──────────────┬───────────────────────────────────┼────────────────┘
                  │ event_bus(线程安全队列)            │
                  ▼                                   ▼
   ┌────────────────────────────┐      ┌──────────────────────────────┐
   │ access_service → auth_fsm  │      │ enroll_service(录入编排,已有)│
   │ FSM 判定:1.5s 窗/黑名单拒/  │      │ 特征→查重(注入比较器)→加密入库 │
   │ per-user 人脸开关→开门+日志  │      │ → library_add 同步特征库      │
   └──────────┬─────────────────┘      └──────────────────────────────┘
              ▼ EV_UI_GOTO_PAGE / 弹窗 / 脸框颜色
   ┌──────────────────────────────────────────────────────────────────┐
   │ UI(ui.c 全局泵 → navigator → presenter_home → page_home setter)  │
   └──────────────────────────────────────────────────────────────────┘
```

### mode 状态机(FSM 状态 → vision 模式,access 每次 FSM 事件后派生并下发)

| FSM 状态 | vision mode | 检索 | 录入缓存 | 脸框 |
|---|---|---|---|---|
| ST_NORMAL 且 match_enabled | DETECT_1N | ✅ | ✅ | ✅ |
| ST_ADMIN_AUTH(菜单入口认证) | DETECT_1N | ✅(role 过滤在 FSM) | ✅ | ✅ |
| ST_VERIFY(选了人脸 1:1) | VERIFY_11(cur_uid) | ❌(1:1 比) | ✅ | ✅ |
| 菜单/用户/待机/结果/其它验证子步 | DETECT_ONLY | ❌ | ✅ | 发布但当前页不渲染 |
| 系统级关人脸(后续接 access_set) | IDLE(不推帧) | ❌ | ❌ | ❌ |

> ST_ADMIN_AUTH 用 DETECT_1N 是落地时的修正:spec-auth §3 要求管理员人脸
> 也能在管理员中 1:N 检索,而 FSM 的 `on_match_1n` 本来就处理 ST_ADMIN_AUTH
> (role≠1 红弹窗 + 5s 重来)。若按"只有 ST_NORMAL 才 1:N",管理员刷脸进
> 菜单这条产品路径会永远走不通,FSM 里那段也成死代码。

双重门禁:vision 层阈值过滤 + FSM 层 match_enabled/黑名单/per-user 开关——
误触发开门在语义上不可能。

### 线程模型(不新增线程)

- camera 轮询线程:PushFrame 异步投递(内部队列满=丢帧,无害);
- ROCKIVA 内部线程:NPU 推理 + 回调;
- 回调出站一律 EVENT_BUS_PUBLISH;UI 经 ui_events 队列由主循环泵。

### 活体接口(B7 留口 / B8 几何法实现)

1. liveness_service.h 增 `liveness_service_on_face(landmarks[], n, quality)`
   (B7 空实现);vision_rockiva 开 106 关键点后在 analyse/det 调用;
2. cfg 增 `liveness_enable`(默认 0);命中发布处门禁;
3. B8:动作状态机(眨眼=EAR 边沿/点头=纵向位移比/摇头·转头=yaw 往返),
   随机 2~3 指令序列,UI 动作引导页,每步 5s 超时,全过置 pass。

> **B7 已落地版本**(2026-09-18):`liveness_service_on_face/pts/quality` +
> `liveness_service_pass()`(恒 true,cfg 开着会打一条"未实现,本次放行"的告警,
> 不静默失效)+ `liveness_service_last_face_age_ms()`(联调观测)。
> 关键点用与后端解耦的 `dg_face_pt_t{int32 x,y}` 万分比坐标传。
> 门禁挂在 1:N 与 1:1 两处发布前(B8 直接换 pass 的实现即可)。

## 0.2 holder 移植方案(已批准)

main.c 手工装配 → holder 注册表(`proto/holder/holder.h`,README 有用法)。
**注册表**(顺序=依赖序;init_fn 统一 `int(void)`,void 服务包一层):

| 模块名 | required | 依赖 | init_fn 包装说明 |
|---|---|---|---|
| event_bus | ✅ | — | event_bus_init |
| tasker | ✅ | event_bus | tasker_init |
| storage | ✅ | tasker | storage_init(路径按 DG_SIM 分支,包 wrapper) |
| config | ✅ | storage | cfg_load wrapper(json 路径按 DG_SIM) |
| capture | ❌ | config,camera | capture_service_start(void→int wrapper) |
| camera | ❌ | config | camera_init wrapper(路径 /dev/video51) |
| vision_service | ✅ | event_bus | vision_service_start |
| vision_backend | ❌ | vision_service,camera | **vision_backend_start 改 int 返回**;成功/失败内部 holder_set_module_state("vision_backend", READY/ERROR),last_error 写"缺模型/Init 失败" |
| access / enroll / liveness | ✅/❌/❌ | tasker | 各 service_start wrapper |
| web / mdns | ❌ | config | web_server_start / mdns_start |
| ui | ❌ | display | ui_init wrapper(失败仅告警,web 路径照常) |

- `holder_init_all(true)`:required 失败 → 退出(S60 3s 拉起重试);❌ 失败 →
  记 ERROR 继续(摄像头/视觉/UI 挂了门禁/web 仍可用)。
- 运行期健康:web 后续可加 /api/health 遍历 `holder_get_module_info`。
- 主循环(camera_poll + ui_poll)保持不变。

**落地差异(2026-09-18 实施,已上板验证)**:

- 表里"camera 依赖 config"写成 `{config}`;"ui 依赖 display"改成 `{config}`——
  display 由 `ui_init` 内部初始化、没有独立模块,若声明依赖不存在的模块名,
  holder 会把 ui 直接判 ERROR(依赖未满足)。ui 的真实依赖只有 cfg(语言/主题)。
- `vision_backend` 内部**不再**调 `holder_set_module_state`:holder_init_module
  会拿 init_fn 的返回值覆盖状态与 last_error(源码:ret==0 → READY,非 0 → ERROR
  + "Initialization failed"),模块里自我注册是冗余的双层耦合。失败细节留在
  `[VISION]` 的 DG_LOGE 里(实测会打出缺哪些模型文件)。
- 板上实测状态表:14 模块,必需全 READY;`vision_backend = ERROR(Errors: 1)`
  时应用照常运行(相机/UI/web/触摸都起)——降级语义符合设计。

## 0.3 当前快照(2026-09-18 晚,§2 ①②③⑤⑦⑧⑨ 已完成并上板)

- 交叉编译零警告;`dg-test` 17/17(含新增 `test_vision_mode`);`--tsan` 17/17
- 板上跑的是本次 B7 二进制:`/etc/init.d/S60doorguard` 已带
  `export DG_IVA_MODEL_DIR=/usr/lib`(dg-deploy 自动同步)
- 板上模块状态:全 READY,**除 vision_backend = ERROR**(人脸模型缺失,见 §2.4)
- 待用户人工回归:UI 待机唤醒/菜单四入口/用户管理/中英切换(UI 重构后未全测)

## 1.1 首个验证点(人脸模型到位后第一件事!)

**DG_FEATURE_MAX=512,而 ROCKIVA_FACE_FEATURE_SIZE_MAX=4096**——若实际特征
超过 512B,当前代码**不再静默丢弃**:analyse 回调会打
`特征 N B > DG_FEATURE_MAX(512),已丢弃——请提高 proto/types.h DG_FEATURE_MAX 后重编译`
(每进程最多 3 条)。正常路径也会打一条 `人脸特征长度 N B(上限 512)`(长度变化时再打),
所以**上板看这一行就知道是否要提 DG_FEATURE_MAX**(表是 BLOB,提了免迁移)。

## 1. 已完成(代码在工作树/已提交)

- **modules/vision/vision_rockiva.c**(重写,~330 行):ROCKIVA_Init(VIDEO 模式,
  modelPath=DG_IVA_MODEL_DIR 默认 /usr/lib)→ FACE_Init(NORMAL+抓拍+识别+关键点,
  detectScore=60,OPT_FAST)→ 摄像头线程喂帧(零拷贝包 NV12)→
  det_cb(最大脸,万分比→像素→旋转 90° 映射竖屏,150ms 节流)→ EV_VISION_FACE_BOX;
  analyse_cb(qualityResult==OK 的 faceAnalyseInfo.feature)→ 缓存(3s 新鲜窗,
  供录入)+ SearchFeature("dg_users") → 命中≥cfg face_dup_threshold 且
  db_user_get 存在 → EV_VISION_MATCH_1N;启动 db_user_iter_face 全量装载特征库;
  lib_add/lib_del;storage_set_feature_cmp 注入 FeatureCompare(1=重复)。
  录入抓取:EV_VISION_CAPTURE_REQ → 提交近 3s 缓存特征。
- **hal/camera**(camera.h/.c):`camera_set_nv12_listener(on_frame, NULL)` +
  `camera_nv12_release(frame_id)`;帧给视觉后标 busy,release 回调里延迟 QBUF
  (4 缓冲,ROCKIVA 持有期间驱动不回填)。
- **modules/vision/vision_backend.h**(新增,2026-09-18 晚):视觉后端**契约**
  (ops:name/model_tag/has_landmarks/start/lib_add/lib_del/compare/on_mode)。
  服务层不再认识 ROCKIVA:`vision_backend_register()` 由 app/main.c 在装配时调用
  (DG_SIM 分支注册 sim,否则注册 rockiva),`vision_backend_start()` 按
  env `DG_VISION_BACKEND` > cfg `face.backend` > 第一个注册的选择,并统一
  注入比较器/库转发/模式钩子/口径校验。**旧接口 `vision_service_set_lib_ops`
  与 `vision_service_set_mode_hook` 已删除**(改由 ops 提供),新写后端只看
  `modules/vision/README.md` 的契约表与"新增后端 4 步"。
- **特征口径(model_tag)**:换模型 = 换特征空间,库里旧特征不可比。生效 tag =
  cfg `face.model_tag` > 后端自带;首次启动登记进 `device_config.face_model_tag`,
  不一致 → ERROR 日志 + **屏蔽命中**(宁不开门不错开门),直到重录人脸并把该键
  改成新值。板上已实测登记行 `特征口径登记 face_model_tag=rockiva-face-v1`。
- **modules/vision/vision_service.c**:后端注册表 + 选择 + 一次性接线 + 口径校验
  (服务层与模型彻底解耦)。
- **enroll_service.c**:录入入库成功后 `vision_service_library_add`;删除后
  `vision_service_library_remove`。
- **CMake**:dg_vision(非 sim)的 rockiva/dg_storage/dg_config/dg_camera/dg_liveness
  链接块放在 dg_camera 定义**之后**(CMake 目标顺序:放前面会退化为 -l 直链失败)。
- 交叉编译:零警告。**PC sim 不受影响**(mock 后端)。

### 本轮补完(2026-09-18 晚)

- **holder 接入**(§2.1):`app/main.c` 重写为注册表装配(14 模块),
  `vision_backend_start` / `vision_service_set_mode` 等接口齐备。
- **mode 联动**(§2.2):`EV_VISION_SET_MODE` 事件 + `vision_service_set_mode`
  + 后端钩子 `ops->on_mode`(VERIFY_11 时装目标特征;原 `set_mode_hook` 已并入契约);
  access 侧 `sync_vision_mode()` 由 `fsm_feed()` 每次 FSM 事件后调用。
  **关键补线**:此前没有任何地方把 `EV_VISION_VERIFY_11` 送进 FSM
  (FSM 的 `FSM_EV_VERIFY_11` 分支早就写好了)→ access_service 新增
  `on_verify_11` 订阅搬运。1:1 **只在通过时发布**(失败由 FSM 子步 5s 超时收口,
  避免逐帧低分把流程提前打死)。
- **活体留口**(§2.3):`liveness_service_on_face/pass/last_face_age_ms` 实现;
  det/analyse 两侧回灌 106 点;两处命中发布前门禁 `liveness_enable && !pass → 不发`;
  模块 README 记录 B7 语义与 B8 计划。
- **人脸长度自诊断**(§1.1):正常打 `人脸特征长度 N B`,超限打 ERROR 提示提宏。
- **EV_VISION_FACE_LOST**(新增):原来板上后端从不发它,人走开后主页黄框会一直
  挂着(FSM 的 `FSM_EV_FACE_LOST` 与 UI 的 `page_home_clear_facebox` 都在等它)。
  现按"有→无"边沿发;IDLE 切换时也补发一次(此后没有回调了)。
- **0.1:N/1:1 用两个阈值**:检索/比对接受用 cfg `face_match_threshold`
  (json `face.match_threshold`,默认 0.42),录入查重用 cfg `face_dup_threshold`
  (0.90)。此前用 0.90 当检索阈值太严,1:N 基本不可能命中;
  `device.json` 里 team 早就写了 `match_threshold: 0.42`。
  板上调阈值依据:analyse 里每 2s 一条 `1:N 最高分 X.XXX(阈值 Y)` 节流日志。
- **测试**:新增 `tests/test_vision_mode.c`(模式联动回归,抓出了上面那条漏线);
  `dg-test` 修好(测试构建走 sim 视觉后端)。
- **CMake 清理**(§2.9):删掉 SDK_ROOT 的 include/lib64 残留,可执行文件里
  不再烧 `RPATH=/external/iva/...`(readelf 已验证);旧的 `-L/external/...`
  在板上会触发一次 ENOENT open(无害但难看)。

## 2. 待做(按序)

1. ~~holder 接入~~ **✅ 完成**(见 §1 本轮补完;落地差异见 §0.2)。
2. ~~mode 切换~~ **✅ 完成**(4 模式;ENROLL_CAPTURE 未做——录入取缓存与
   DETECT_ONLY/DETECT_1N 并存,不需要单独模式)。
3. ~~活体留口~~ **✅ 完成**(B8 只填算法)。
4. **板上人脸模型**(需用户操作,唯一阻塞项):板上只有 /usr/lib/object_detection_v3_cls8.data
   (前级检测,ROCKIVA_Init 靠它成功),**人脸模型缺失**。strace 实测 FACE_Init
   会找:`/usr/lib/face_landmark5.data`(或 `.rknn` / `libface_landmark5.so`)、
   `/usr/lib/face_quality_v2.data`(或 `.rknn` / `libface_quality_v2.so`),
   后续还会要识别模型。VM:
   `~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008/external/iva/
   librockiva/rockiva-rk3576-Linux/models/rockiva_data_rk3576/*.data`
   → 整目录 `scp` 到板 `/usr/lib/`(至少 face_landmark5.data、face_quality_v2.data,
   建议全拷)。缺模型时 FACE_Init 返回 -1,日志 ERROR 明示,**系统降级不崩**。
   模型清单/校验和落 `door-guard/models/`(README+sha256,二进制不入 git)。
   后续固件(B10)应在 buildroot 的 IVA 包里带上这些模型,不再手工拷。

   **2026-09-21 补充(重要)**:照此路径排查后发现,**本版 SDK 快照根本没有
   rk3576 的人脸模型**——`external/iva/models/rockiva_data_rk3576` 与
   buildroot 构建产物两处都只有前级检测 `object_detection_v3_cls8.data`,
   `iva.tar` 内亦无;人脸件只存在于 rk3588/rv1126 目录(跨芯片不可用,
   别拷)。**须走 Kickpi 厂商渠道要 rk3576 的 rockiva 人脸模型包**。
   同期主线已切换为自组 rknn(RetinaFace + ArcFace,已上板跑通),
   无需 ROCKIVA 模型即可交付检测+识别;本路线保留为备选。
5. **部署联调**(模型到位后):`source env/env.sh && dg-build && dg-deploy`。
   看板日志(/var/log/door-guard.log):`ROCKIVA 就绪` + `人脸特征长度 N B` →
   用户管理页录入人脸(正对镜头 3s 内)→ 日志 `特征库装载` → 主页举脸:
   脸框跟随(yellow)→ 命中(green+开门+日志 `1:N 命中 x‰`)。脸框方向不对改
   rect_to_screen(90↔270 公式);分数偏高/偏低调 cfg `face_match_threshold`。
   排查工具:`DG_IVA_LOG=3 /root/door-guard` 看 ROCKIVA 找模型的路径。
6. **收尾**:DEVLOG/PROJECT_PLAN 已更新;本文件已改为"完成待联调";push 见 git log。
7. ~~S60 加 env~~ **✅ 完成**(`export DG_IVA_MODEL_DIR="${DG_IVA_MODEL_DIR:-/usr/lib}"`)。
8. ~~vision_backend_start 改 int~~ **✅ 完成**(4 处:vision_service.h / vision_sim.c /
   vision_rockiva.c / main.c 包装)。
9. ~~链接残留清理~~ **✅ 完成**(RPATH 已清,见 §1 本轮补完)。
10. **用户回归清单**(§0.3):待机唤醒/菜单四入口/用户管理/中英切换 + B7 新流程。

## 3. API/坑备忘(新会话勿重踩)

- **后端可插拔**:服务层只认 `vision_backend.h` 的 ops;要换/加后端(ROCKIVA→
  rknn 开源模型等)看 `modules/vision/README.md`,别去改 vision_service/FSM/存储。
  换模型(含同框架换 .data 文件集)必须同时改 `face.model_tag`,否则命中被屏蔽
  (这是有意的安全阀,不是 bug)。

- **两个枚举**:函数返回 `RockIvaRetCode`(成功=**ROCKIVA_RET_SUCCESS**);
  回调 status `RockIvaExecuteStatus`(成功=**ROCKIVA_SUCCESS**)。混用
  -Wenum-compare 警告。回调比较用 `(int)status != ROCKIVA_SUCCESS`。
- `RockIvaRectangle` = {topLeft, bottomRight},万分比 int16(0~9999),**非 x/y/w/h**。
- `RockIvaHandle=void*`:先 `ROCKIVA_Init(&h, ROCKIVA_MODE_VIDEO, &InitParam, NULL)`
  (InitParam.modelPath=模型目录,logLevel,cameraType=ONE,imageInfo=NV12 1280x720),
  再 `ROCKIVA_FACE_Init(h, &faceParams, callbacks)`(按值传 h)。
- `ROCKIVA_SetFrameReleaseCallback` **必须设**,否则 V4L2 缓冲永不归还 → 断流。
- 特征:`faceAnalyseInfo.feature/featureSize`(典型 ~几十~几百字节,
  DG_FEATURE_MAX=512 够);库操作 `FeatureLibraryControl(lib,INSERT/DELETE,
  faceIdInfo[1],1,feat,featSize)`;检索 `SearchFeature(lib,feat,featSize,1,1,&sr)`,
  sr.faceIdScore[0].score 0~1.0。
- 头文件:sysroot `usr/include/rockiva/`(rockiva_face_api.h 532 行,含全部契约);
  板上库 /usr/lib/librockiva.so + librknnrt.so。
- 事件契约:EV_VISION_FACE_BOX/LOST(ev_face_box_t x,y,w,h,state 像素,竖屏)、
  EV_VISION_MATCH_1N(ev_match_t matched/user_id/user_name/role/score_permille)、
  EV_VISION_CAPTURE_REQ(ev_capture_req_t user_id,seq)、EV_VISION_FEATURE。
- storage:`db_user_iter_face(fn(uid,plain,len,ud))` 明文遍历;
  `storage_set_feature_cmp(fn, NULL, ud)`,fn 返回 1=重复 0=不重复 <0=错。
- vision_backend_start 已改由 camera 喂帧;**enable_mock 参数板上恒 false**。
- ui/ 分层与铁律见 door-guard/ui/README.md(事件泵在 ui.c,勿放页面)。
- 板:192.168.2.95,SSH root;日志 /var/log/door-guard.log;
  服务 /etc/init.d/S60doorguard{start|stop|restart}(A/B 槽 OTA 已上线)。
- push 一律 `git push origin master`(GitHub);上下文压缩交接惯例见本文件。
- **缓冲饿死坑(已修,别回退)**:on_frame_push 未就绪/推送失败必须立即
  camera_nv12_release(frame_id),否则 4 缓冲耗尽相机永久断流。
- 链接顺序坑(已修):dg_vision 的 target_link_libraries 若放在 dg_camera
  定义之前,CMake 退化为 -l 直链,undefined reference。
- EV_VISION_VERIFY_11:FSM 侧已备好(auth_fsm.c:273 转 VERIFY_RESULT),
  vision 的发布路径见 §1;access 侧必须有人把它转成 FSM_EV_VERIFY_11
  (现已由 access_service `on_verify_11` 订阅搬运)。**勿在 FSM 里加东西**。
- 链接行残留 `-L/external/iva/...` 已清(§2.9),readelf 确认无 RPATH 残留。
- PC 端(sim):vision_sim mock 不依赖 camera NV12 出口;camera_set_nv12_listener
  在 sim 的 camera_sim.c 无实现——sim 不调它即可,勿在公共路径调用。
  测试构建(`DG_BUILD_TESTS`)走 sim 视觉后端:**板上后端只在交叉编译里编**
  (宿主没有 rockiva 头,曾因此让 dg-test 整体编不过)。
- **人脸模型文件清单(板上实测 2026-09-18,strace 得来)**:FACE_Init 在
  modelPath 下依次找 `face_landmark5.data`(.rknn / libface_landmark5.so 备选)、
  `face_quality_v2.data`(.rknn / libface_quality_v2.so 备选),之后还有识别模型;
  缺任一 → 返回 -1。`object_detection_v3_cls8.data` 只够 ROCKIVA_Init 成功。
- **批量改代码的坑**:`sed -i 's/auth_fsm_handle(&s_fsm, /fsm_feed(/g'` 会把
  包装函数 `fsm_feed` 自己的函数体也替换掉 → 无限递归 SEGFAULT(靠 test_e2e 抓到)。
  批量替换后务必回看被改函数本身。
- **holder 语义**:`holder_init_module` **用 init_fn 的返回值覆盖**状态与
  last_error(ret==0 → READY,非 0 → ERROR + "Initialization failed"),
  所以模块内部再调 `holder_set_module_state` 是冗余的;依赖未满足的模块在
  `holder_init_all` 收尾时统一置 ERROR(依赖模块名写错 = 该模块永远 ERROR,
  例如 ui 声明依赖不存在的 "display")。
