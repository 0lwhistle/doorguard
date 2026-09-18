# B7 人脸识别 — 交接文档(2026-09-18,上下文压缩交接)

> 读本文前先读 `docs/DEVLOG.md` 顶部两条(B6 相机/OTA 已完成)。
> 本文是 B7 的唯一交接事实源:已完成/待做/坑/API 备忘全在此。

## 0. 一句话状态

B7 代码主体已写完且**交叉编译零警告**(rockiva 后端、相机 NV12 出口、
特征库同步、录入挂钩、CMake),**未推板未实测**;剩:holder 接入、
mode 切换、活体留口、板上模型文件(需用户从 VM 拷)、部署联调。

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

### mode 状态机(FSM 状态 → vision 模式,access tick 1s 联动)

| FSM 状态 | vision mode | 检索 | 录入缓存 | 脸框 |
|---|---|---|---|---|
| ST_NORMAL 且 match_enabled | DETECT_1N | ✅ | ✅ | ✅ |
| ST_VERIFY(选了人脸 1:1) | VERIFY_11(cur_uid) | ❌(1:1 比) | ✅ | ✅ |
| 菜单/用户/待机/结果 | DETECT_ONLY | ❌ | ✅ | 发布但当前页不渲染 |
| 系统级关人脸(后续接 access_set) | IDLE(不推帧) | ❌ | ❌ | ❌ |

双重门禁:vision 层阈值过滤 + FSM 层 match_enabled/黑名单/per-user 开关——
误触发开门在语义上不可能。

### 线程模型(不新增线程)

- camera 轮询线程:PushFrame 异步投递(内部队列满=丢帧,无害);
- ROCKIVA 内部线程:NPU 推理 + 回调;
- 回调出站一律 EVENT_BUS_PUBLISH;UI 经 ui_events 队列由主循环泵。

### 活体接口(B7 留口 / B8 几何法实现)

1. liveness_service.h 增 `liveness_service_on_face(landmarks[], n, quality)`
   (B7 空实现);vision_rockiva 开 106 关键点后在 analyse/det 调用;
2. cfg 增 `liveness_enable`(默认 0);1:N 命中发布处门禁;
3. B8:动作状态机(眨眼=EAR 边沿/点头=纵向位移比/摇头·转头=yaw 往返),
   随机 2~3 指令序列,UI 动作引导页,每步 5s 超时,全过置 pass。

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
- **vision_service.h/.c**:`vision_service_set_lib_ops/add/remove`(库维护转发,
  未注册返回 DG_ERR_NOT_INIT)。
- **enroll_service.c**:录入入库成功后 `vision_service_library_add`;删除后
  `vision_service_library_remove`。
- **CMake**:dg_vision(非 sim)的 rockiva/dg_storage/dg_config/dg_camera 链接块
  放在 dg_camera 定义**之后**(CMake 目标顺序:放前面会退化为 -l 直链失败)。
- 交叉编译:零警告,22 目标全过。**PC sim 不受影响**(mock 后端)。

## 2. 待做(按序)

1. **holder 接入**(用户已批准):main.c 手工装配改 holder 注册表
   (`proto/holder/holder.h`,README 有完整用法)。模块与依赖:
   event_bus → tasker → storage → config(cfg_load 的 json 路径按 DG_SIM 分支,
   包一层 int(void) init_fn)→ capture → vision_service → vision_backend
   (vision_backend_start 改 int 返回,内部成功/失败调
   holder_set_module_state("vision_backend", READY/ERROR),required=false
   ——缺模型降级不阻塞)→ access/enroll/liveness → web → mdns → ui(required=false)。
   `holder_init_all(true)`;主循环(camera_poll+ui_poll)保持不变。
2. **mode 切换**(接口已在方案定稿):vision_service 增
   `vision_service_set_mode(mode, uid)`;枚举
   DETECT_1N / DETECT_ONLY(脸框+录入缓存,不检索)/ VERIFY_11(uid)/
   ENROLL_CAPTURE / IDLE(不推帧)。vision_rockiva 每帧/每回调按 mode 分支。
   **联动点**:access_service 的 on_access_tick(1s,已有)读公开字段
   s_fsm.state / match_enabled / cur_uid:
   - ST_NORMAL 且 match_enabled → DETECT_1N
   - ST_VERIFY(FACE_1V1 步)→ VERIFY_11(cur_uid);检索命中改发
     EV_VISION_VERIFY_11(**FSM 已预留该事件**,auth_fsm.c:273 直接转
     FSM_EV_VERIFY_RESULT,勿重复实现)
   - 其他页(菜单/用户/待机)→ DETECT_ONLY(录入照常,检索关,无误触发)
   - 系统级关人脸(后续接 access_set)→ IDLE
3. **活体留口**(B8 实现算法):liveness_service.h 增
   `liveness_service_on_face(landmarks, n, quality)`(B7 空实现);
   vision_rockiva analyse/det 处调用(landmarks 已在 faceInfo,开 106 点
   faceLandmarkEnable=2);cfg 加 liveness_enable(默认 0);
   1:N 命中发布处加门禁 `cfg liveness_enable && !pass → 不发`。
4. **板上人脸模型**(需用户操作):板上只有 /usr/lib/object_detection_v3_cls8.data
   (前级检测),**无人脸模型**。VM:
   `~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008/external/iva/
   librockiva/rockiva-rk3576-Linux/` 下 model 目录 → scp 到板 /usr/lib/。
   缺模型时 ROCKIVA_Init 失败,日志 ERROR 明示,系统降级不崩。
5. **部署联调**:`source env/env.sh && dg-build && dg-deploy`(自动停服务推送
   拉起)。看板日志(/var/log/door-guard.log):`ROCKIVA 就绪` → 用户管理页录入
   人脸(正对镜头 3s 内)→ 日志 `特征库装载` → 主页举脸:脸框跟随(yellow)→
   命中(green+开门+日志 1:N 命中 x‰)。脸框方向不对改 rect_to_screen
   (90↔270 公式);相似度偏高/偏低调 cfg face_dup_threshold。
6. **收尾**:DEVLOG B7 条目 + 本文件更新为"已完成" + push GitHub。

## 3. API/坑备忘(新会话勿重踩)

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
