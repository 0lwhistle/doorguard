# 开发日志(DEVLOG)

> 记录约定:每次会话/每个工作日**追加**新条目(最新在最上),写清"做了什么 / 结论 / 踩了什么坑"。
> 本日志记"过程与坑",当前状态看 `DEV_HANDBOOK.md`,方案看 `PROJECT_PLAN.md`。

---

## 2026-09-18 验证按钮 + 菜单业务打通;修掉"新机无管理员进不去菜单"死锁

- **背景**:上一轮 UI 重构(Phase 7)把 page_home 瘦身成纯渲染后,FSM 动作到
  UI 控件那一跳没人补 —— `FSM_ACT_ASK_UID`/`FSM_ACT_SHOW_METHODS` 无处理者,
  **点"验证"= 静默 5 秒弹"验证失败"**(ID 框和方式选择永远不出现)。
  这轮把它补齐并加回归测试

- **新增 UI 请求事件**(proto/events.h,服务层→UI,UI 只渲染):
  `EV_UI_ASK_UID` / `EV_UI_INPUT_PWD{uid}` / `EV_UI_PICK_METHOD{auth_flags}` /
  `EV_UI_RESULT{ok,reason,user_name,not_admin}` / `EV_UI_HINT_CLEAR` /
  `EV_UI_FACEBOX{state,box}`;bridge 侧配套 `bridge_uid_submit/pwd_submit/
  method_pick/cancel`,弹窗取消统一回 `EV_UI_BTN{BACK}`
- **验证流程 UI**(presenter_home):ID 输入框 → 方式选择(只列开启的,仅一种
  直接进)→ 密码框(掩码,uid 回填)→ 结果弹窗;**失败文案按 reason 映射**
  (用户不存在/密码错误/该方式未开启/全部验证方式已关闭/摄像头未就绪;
  陌生人·黑名单·超时统一"验证失败";管理员入口另有"非管理员")
- **脸框颜色两个来源**:视觉后端只发黄色检测框,FSM 命中/失败经
  `EV_UI_FACEBOX` 改色(位置仍用检测框,不抖)。此前 FSM 的绿/红框到不了 UI
- **菜单业务死锁修复**(用户提的业务漏洞):点"菜单"时 access_service 先查
  `db_user_count_role(ADMIN)` 喂给 FSM(FSM 不碰 DB);**库里没有管理员 →
  免认证直接进菜单 + 提示"未设置管理员,请先添加管理员"**(新机/管理员被删光);
  人数未知(-1,查询失败)按"有管理员"保守处理。新增 storage 接口
  `db_user_count_role()`
- **管理员入口补 spec §3 缺项**:管理员态点"验证" → 通过后校验 role:
  管理员 → **进菜单(不开门)**;非管理员 → 红弹窗"非管理员" + 停留重试
- 其他修正:未开启方式被拒(reason=6,防陈旧弹窗注入);子步超时日志用当前
  子步方式(原来固定 PWD);提示条在回普通/待机时清掉(HINT_CLEAR 终于有收发方);
  取消流程不写日志(spec §4.6)
- **测试**:新增 `tests/test_verify_flow.c`(服务层端到端:把模拟器手点流程
  自动化,断言每一步 UI 该收到的事件)+ test_auth_fsm 新增 F12(菜单入口/角色/
  取消/未开启方式)+ test_storage 补按 role 计数;dg-test 19/19,--tsan 19/19
- **模拟器可全流程演示**:vision_sim 的 mock 改为**按工作模式产出**
  (DETECT_ONLY 只画框、DETECT_1N 命中/离开、VERIFY_11 按目标 uid 回通过),
  PC 上不接人脸模型也能把"验证 → ID → 方式 → 密码/1:1 → 开门"走完
- **踩坑**:新增中文文案后 `test_i18n` 报"字体缺字形"(非/先)→ 改 lang json
  必须跑 `ui/font/gen.sh` 重生成字库;注释里 ASCII 引号包中文会被判字符串

---

## 2026-09-18 B7 补:视觉后端做成可插拔(契约/注册表/特征口径)

- **动机**(用户提):后续想换模型,包括 SCRFD+ArcFace 一类开源模型。
  原来后端由 CMake 编译期写死,换模型只能改代码重编 → 抽出正式契约

- **新增 `modules/vision/vision_backend.h`**:`vision_backend_ops_t`
  {name / model_tag / has_landmarks / start / lib_add / lib_del / compare /
  on_mode} + 注册与选择 API + **8 条硬性义务**(特征上限显式报错、出站只走总线、
  命中前必须查口径一致、帧必须归还缓冲……)。服务层/UI/FSM/存储对后端零依赖

- **vision_service 变纯服务层**:注册表(选择序 env `DG_VISION_BACKEND` >
  cfg `face.backend` > 第一个注册的)+ 一次性接线(比较器注入 storage、
  lib ops 转发、模式钩子、口径校验);删掉 `set_lib_ops/set_mode_hook`(旧接缝)

- **口径校验(换模型的安全阀)**:生效 tag = cfg `face.model_tag` > 后端自带
  `model_tag`;首次启动登记进 `device_config.face_model_tag`,不一致 → ERROR
  日志 + **屏蔽 1:N/1:1 命中**(宁可不开门不可错开门),提示重录人脸后改该键。
  换 ROCKIVA 的 .data 文件集属于"同框架换模型",也靠这个 tag 兜住

- **新 cfg(JSON-only,不进 DB)**:`face.backend` / `face.model_dir` /
  `face.model_tag`(默认空 = 用第一个注册的后端与其自带口径,这样同一份
  device.json 在 PC(sim)与板上(rockiva)都成立)

- **测试**:新增 `tests/test_vision_backend.c`(假后端驱动:注册/选择/转发/
  比较器注入/口径拦截/启动失败上报);`dg-test` 18/18,`--tsan` 18/18

- **文档**:`modules/vision/README.md`(契约逐字段义务 + 换模型两条路 +
  rknn 开源模型清单/差异点:embedding 2048B 要提 DG_FEATURE_MAX、余弦比较器、
  关键点模型对应 B8 活体)

- **板上实测**:`特征口径登记 face_model_tag=rockiva-face-v1`(sqlite3 查
  device_config 已见),后端摘要行 `后端 rockiva(model_tag=...,关键点=有)启动失败(降级)`

---

## 2026-09-18 B7 续作:holder 接入 + 模式联动 + 活体留口(已上板,差人脸模型)

- **① holder 接入**:`app/main.c` 手工装配改注册表(14 模块,`/usr/lib` 相机节点等
  参数走静态变量)。板上实测:必需模块全 READY,`vision_backend` = ERROR 时
  系统照常起(摄像头/UI/web 都在)——降级路径就是设计要的样子
- **② mode 联动**:新事件 `EV_VISION_SET_MODE`(access → vision,模块间仍只走总线);
  `vision_service_set_mode(DETECT_1N/DETECT_ONLY/VERIFY_11/IDLE)` + 后端钩子;
  access 每次 FSM 事件后派生模式(普通/管理员态 1:N,1:1 子步带 cur_uid,
  其余 DETECT_ONLY)。**补了一处漏线**:没人把 `EV_VISION_VERIFY_11` 送进 FSM
  (FSM 侧分支早就有),现由 access 订阅搬运——test_vision_mode 抓出来的
- **③ 活体留口**:`liveness_service_on_face`(106 点)+`liveness_service_pass`;
  `cfg liveness_enable`(默认 0)门禁挂在两处命中发布前。B7 pass 恒 true
  (cfg 开着会打一条"未实现,本次放行"的告警,不静默)
- **坑/发现**:
  1. **test 构建本来就是坏的**:`dg-test` 里 dg_vision 编 rockiva 后端(宿主无
     rockiva 头)→ 改 `if(DG_SIM OR DG_BUILD_TESTS)` 走 sim;dg-test 现 17/17
  2. `sed` 批量替换 `auth_fsm_handle(&s_fsm,` → `fsm_feed(` 把 `fsm_feed` 自己
     的函数体也换了 → 无限递归 SEGFAULT(test_e2e 当场抓到)
  3. strace 板上跑:**ROCKIVA 人脸模型缺 `face_landmark5.data` /
     `face_quality_v2.data`**(B4 rootfs 只装了 object_detection_v3_cls8.data)
     → `ROCKIVA_FACE_Init` 返回 -1;`DG_IVA_LOG=3` 可看它找文件的路径
  4. 清掉 CMake 里 SDK_ROOT 残留(-L/external/iva/...),二进制里烧进的
     `RPATH=/external/iva/...` 一并消失(板上曾见 ENOENT 打开,无害但难看)
- **未做**:板上人脸模型要用户从 VM `models/rockiva_data_rk3576` 拷 /usr/lib,
  之后才算完成 §2.5 联调(录入人脸 / 1:N 命中 / faceSize 确认)

---

## 2026-09-18 B7 人脸识别:代码主体完成,交接续作(上下文压缩)

- **已写完且交叉编译零警告**:vision_rockiva(检测/检索/录入缓存/特征库同步/
  查重比较器)、camera NV12 出口(延迟归还)、vision_service 库转发、enroll 挂钩、
  CMake rockiva 链接。PC sim 走 mock 不受影响
- **待做**(顺序+全部 API 备忘):见 **docs/tech/B7_FACE_HANDOFF.md**(唯一交接源):
  holder 接入 main.c(已批准)→ mode 切换(DETECT_1N/VERIFY_11/ENROLL/IDLE,
  access tick 联动 FSM 公开字段)→ 活体留口(liveness_on_face+cfg 门禁)→
  板上人脸模型(用户从 VM SDK 拷 /usr/lib)→ 部署联调 → 收尾
- 下次会话:先读 B7_FACE_HANDOFF.md,按 §2 顺序做,勿重踩 §3 的坑

---

## 2026-09-18 UI 重构:MVP 分层(学 ESP32 ovs 工程)

### 动机与根因
- 用户报"待机点击不回主页":事件泵长在 page_home 定时器里,进待机→home 销毁
  →泵停→唤醒事件(EV_UI_GOTO_PAGE)无人处理,FSM 醒了页面永远不切(日志实锤:
  有触摸唤醒/有待机唤醒回普通模式,但全日志无一条 open home)
- 结构性缺陷,补丁无解 → 参照用户 ESP32 工程(dockerNow/esp32/programs/ovs)
  的 navigator/bridge/presenters/pages 分层整体重构

### 新结构(细节见 door-guard/ui/README.md)
- **navigator/**:注册表+栈;push(前进)/switch(栈内回退/平级,防 home↔standby
  压爆栈)/back/reload(语言热切);页面描述符含 on_enter/on_exit/on_evt 生命周期
- **bridge/**:唯一后端入口——5 个事件订阅编组进 ui_events 队列;动作出站
  bridge_btn/bridge_touch;只有本层可 include 后端头
- **presenters/**:每页注册+on_evt 渲染+弹窗文案;home 的脸框/提示/结果弹窗逻辑
  从视图剥离
- **pages/**:纯视图(home 只剩画布 33ms 刷帧+setter);ui.c 只做引导+全局泵

### 坑
1. navigator_page_t 初版漏 destroy 字段(实现留了调用,编译才暴露)
2. 事件枚举真名 EV_VISION_FACE_BOX(非 EV_FACE_BOX),想当然必错
3. test_i18n 递归扫描后:①ui/ 注释里 ASCII 引号包中文("死区")被判字面量,
   引用词用「」;②生成的字库 font/ 目录必须排除

### 验证
- dg-build(交叉)/dg-build-pc 零警告;dg-test 16/16;板上部署:BRIDGE 就绪、
  open home depth=1、相机就绪;待机唤醒完整链路待人工最终确认

### 下一步
- 板上人工回归:待机唤醒/菜单四入口/用户管理/中英切换
- B7 ROCKIVA(人脸唤醒链路已备好)

---

## 2026-09-18 应用级 OTA 落地(A/B 槽位 + bash 监听/切换/回滚)

### 做了什么(用户定调:不做系统固件升级,只做应用 OTA)
- ota_service:暂存 /tmp → **/var/lib/door-guard**(持久),新增 .sha256/.ver
  sidecar 供 bash 二次复核
- S60 重写:**/root/dg_app.A|B 双槽 + /root/door-guard 符号链接**;监听循环
  (2s 轮询)sha 复核 → 装非活动槽 → 原子切换 → 自动重启;**连续 3 次秒退
  自动回滚**;exec 失败(126/127)跳过 3A 重启;首次运行自动槽位化(幂等)
- dg-deploy 改为"停服务→推→拉起"(符号链接下推运行中的二进制会 ETXTBSY)

### 坑
1. **监督循环继承 ssh stdout/stderr**:会话断开后往死管道写 → 子 shell 卡死
   假死,回滚逻辑失灵。后台循环必须 `>/dev/null 2>&1 &` 完全脱离会话
2. 坏包秒退循环里反复重启 3A 服务 → ISP 驱动内核崩溃一次(看门狗自愈);
   exec 失败路径跳过 aiq_restart 规避
3. S40 start-stop-daemon 会被残留包装 sh 骗过("already running"),重启
   server 必须连 /tmp/.rkaiq_3A pidfile 一起清

### 端到端实测
- 有效包 v2.0.0:上传→复核→装非活动槽→切换→新版本运行 ✅
- 坏包 9.9.9:秒退 3 次→回滚到好槽;期间内核崩溃一次,看门狗重启后
  符号链接在好槽上自启 ✅(A/B 语义:重启永远落在已知好槽)
- 待机触摸唤醒板上人工实测通过(日志多次 触摸唤醒→待机唤醒回普通模式)

### 下一步
- B7:ROCKIVA 上板(人脸检测→待机人脸唤醒链路已备好);web 视频推流选型

---

## 2026-09-18 待机唤醒修复 + AE 曝光上限(减运动模糊)

- **待机页点不醒**:容器默认 SCROLLABLE,手指稍动即判为滚动,CLICKED 永不触发。
  修复:去滚动标志 + 改用 PRESSED(按下即醒)+ 时钟 label 去可点(消中央死区),
  加 "[STANDBY] 触摸唤醒" 日志便于板上验证
- **画面运动模糊**:室内 AE 拉长曝光所致。`rk_aiq_uapi2_setExpTimeRange` 压曝光
  上限,默认 20ms(1/50s),增益换快门;`DG_AE_MAX_MS` 可调,0=不限
- 人脸唤醒的 FSM 链路已存在(FSM_EV_FACE_DETECTED→wake_up),等 B7 ROCKIVA
  上板后自然生效;板上当前无人脸检测器,触摸是唯一唤醒路径(符合预期)
- 板上验证:AE 上限被 rkaiq 接受(日志确认);待机唤醒待人工点按

---

## 2026-09-18 B6 预览打通:V4L2+RGA+rkaiq(相机画面上屏)

### 做了什么
- camera_board 占位桩 → 真实链路:rkisp-vir2 mainpath(/dev/video51)V4L2 MMAP
  单平面 NV12 1280x720 → RGA 旋转90+转 XRGB → 主页 canvas
- 相机初始化全量后台线程化(camera_init 立即返回)。**黑屏根因**:camera_init
  卡在主循环启动前,lv_timer_handler 不跑,屏幕永远停在黑帧
- 板上无人跑通过此相机(lv_demo 无相机代码),以下全按实测摸索

### 坑(按踩的顺序)
1. rkaiq uAPI2 `sns_ent_name` 是**传感器实体名** `m02_b_imx415 8-0037`
   (查 /sys/class/video4linux/v4l-subdev*/name),传 /dev/mediaN 直接段错误
2. **aiq2.lock 死锁**:server 被 prepare 触发后持锁等"流启动事件",而 client
   init/prepare 都要这把锁;单线程任何顺序都双等。解法=并发会合:取流线程
   延迟 500ms STREAMON,server 见流放锁,prepare 返回
3. cam2 传感器映射**第 3 个虚拟 ISP**(rkisp-vir2=/dev/media5,mainpath=
   /dev/video51),不是想当然的 vir0;换端口重查 media-ctl
4. librga 成功码有两个(SUCCESS=1/NOERROR=2),只认一个把成功当失败
5. V4L2 用 V4L2_PIX_FMT_NV12(单平面);NV12M 是双平面,QUERYBUF EINVAL
6. S60 管理 3A server 必须连包装 sh 一起清(pidfile 残留会骗过 S40 判重)
   + 重启后 server 持锁不放 → S60 每次启动前整体重启 server

### 验证
- 像素级:dump 帧 B/G/R 均值 27.8/54.5/34.6,动态范围 0-255,ASCII 缩略图
  有场景结构(非噪声非黑帧)
- 服务:3A 就绪 + 相机状态:就绪;开关 DG_CAM_ROT(方向)/DG_AIQ=0(裸流)/
  DG_CAM_DUMP(取证),详见 hal/camera/README.md

### 下一步
1. 人工确认:画面方向(不对改 DG_CAM_ROT=270)、3A 曝光观感
2. 认证/录入链路接真实帧(B7 ROCKIVA);web 视频推流选型(MJPEG vs gst)

---

## 2026-09-18 板上自启动 + 触摸输入打通

### 根因与修复
- **程序不自启**:`/etc/init.d/` 里从无 door-guard 脚本(非损坏,是没做过)→
  新增 `board/rootfs-overlay/etc/init.d/S60doorguard`(S60:udev/dhcpcd/dropbear
  之后;监督循环崩溃 3s 拉起;日志 /var/log/door-guard.log);dg-deploy 幂等推送
- **触摸无反应**:display_drm.c 一直没注册任何输入设备(注释"待 B5 接入")。
  新增 `hal/display/touch_evdev.c`:名字(fts/goodix/gt9)+MT 能力兜底自动探测,
  Type-B MT slot 与 legacy ABS_X/Y 双协议,abs 范围→屏幕缩放,
  `DG_TOUCH_SWAP_XY/INVERT_X/INVERT_Y` env 校准(零魔数)
- **坑1**:当前屏触摸 IC 是 **fts_ts**(FocalTech,I2C0-0038,MT 协议),不是手册
  早先记录的 goodix——IC 随屏组装不同,DTS 两驱动共存,换屏免改码
- **坑2**:EVIOCGBIT 成功返回**拷贝字节数>0**,写成 `==0` 致 is_mt 恒假走 legacy,
  而 fts_ts 无 ABS_X/Y → 范围 0..0;改 `>=0` 后 mt=1,范围 0..720/0..1280
- **坑3**:板上语言包从未部署(/root/ui/lang 缺失)→ dg-deploy 现随二进制推送;
  `-r` 前先停 S60 服务,避免双实例抢 DRM/SQLite
- 顺带确认:以太网开机自启联网正常(S41dhcpcd)——B5 时期"eth0 不自启"结论已过时

### 验证
- 宿主:dg-test **16/16**(新增 test_touch_evdev 解析单测:MT 按下/移动/抬起、
  双槽跟随、legacy、缩放、校准、越界钳制、槽号饱和)零警告;dg-build 零警告
- 板上:远程重启后 door-guard **自启成功**(ps 448)、event1 打开(mt=1)、
  DRM 渲染、web 8080 OK
- ⏳ 待人工:手指点按校验坐标方向;若偏转,在 S60 脚本启动前 export DG_TOUCH_*

### 下一步
1. 手指实测触摸方向/灵敏度,结论回写 DEV_HANDBOOK §2
2. rootfs-overlay 并入 VM SDK overlay,下版固件自带自启
3. 门控 GPIO 对拍(仍待引脚确认,悬置)

---

## 2026-09-18 目录整理 + 上 GitHub(历史重写,remote 变更)

### 做了什么
- 清掉空残留目录:根 `hal/`、`door-guard/{env,docs,lang}`;删 Zone.Identifier 垃圾;
  `NEXT_SESSION_PROMPT/IDLE_TASK_PROMPT` 移入 `docs/prompts/`(README 链接同步)
- mongoose 7.23 正式 vendor:`third_party/mongoose/`(仅 amalgamated 源+LICENSE,
  删 13MB zip)。**GPLv2/商业双许可,闭源商用需商业授权,接入前先定许可路线**;
  现阶段未接线,web 仍是 civetweb
- 新增根 `.gitignore`;`deliverables/` 大二进制(固件镜像/工具链 tar,~1.2GB)与
  `sim/data/` 运行时产物(db/-wal/-shm/dg.key 首跑自建)退出跟踪,磁盘保留
- **git 历史重写**(git-filter-repo 剥离 8 个大 blob):`.git` 708MB→12MB,
  全部 commit hash 已变(旧头 662515e → 新历史)。仓库仅 40 commits
- 编译验证全过:dg-build / dg-build-pc / dg-test / dg-test --tsan 均 15/15、零警告
- remote:**origin = GitHub `git@github.com:0lwhistle/doorguard.git`(master 已推)**;
  旧 Gitea 改名 `gitea` 保留(旧历史=固件镜像唯一异地副本,勿 force 覆盖)

### 坑
- GitHub 单文件 100MB 硬上限:不剥历史直接推必被拒(只删工作区文件不够,blob 在史中)
- filter-repo 结尾会 reset --hard:先 `git rm --cached` 退跟踪再重写,磁盘文件才保得住
- **Gitea 停在旧历史,VM 勿直接 pull**(会撞回旧史),VM 切换步骤见 DEV_HANDBOOK §7

### 下一步
1. VM 直连 GitHub 后 `git fetch origin && git reset --hard origin/master`
2. 板恢复后:重推 door-guard → gpio 对拍 → web/mDNS 板上实测(前次遗留)
3. mongoose 是否替换 civetweb:先定 GPL/商业许可路线再动

---

## 2026-09-18 Phase 10 收尾:总验收自测 + 文档

### 总验收清单自测(任务清单§4)

1. ✅ dg-build / dg-build-pc 零警告;dg-test 15 用例全绿(常规+tsan,428 断言)
2. ✅ 模拟器:三页面+菜单四子页渲染与交互验证(截图 docs/img/);中英切换机制
   已实现(i18n_set_language → 全页重建),待板上触摸可用后人工复核
3. ⚠️ 板上:主页可显示(DRM,截图 board-home-phase6.png)、门控/触摸
   受硬件确认阻塞(见下);数据库落盘可查(板上 storage ready 日志)
4. ✅/⚠️ web:登录/日志/设备信息/NTP/OTA 全功能验收通过(tests/web/web_test.sh
   14 项,宿主);板上实测待板恢复;mDNS WSL2 NAT 受限(hosts 兜底)
5. ✅ OTA:上传→sha256 校验→暂存闭环(脚本 dg-ota-upload);真刷待分区方案
   (docs/tech/OTA_PLAN.md 已写方案)
6. ✅ 各模块 README 齐(proto 三组件/config/storage/access/ui/gpio/uart);
   DEVLOG 条目齐;待硬件确认清单齐;全部工作已 push

### 板失联事故处理记录

- 上午 gpio 对拍未知引脚致板上挂起,需物理断电恢复(操作前已评估并记录风险,
  但低估了方向写入影响面——后续物理操作一律先查 pinctrl 复用)
- 板恢复后待办:重推 door-guard → gpio 对拍(确认引脚)→ web/mDNS 板上实测

### 移交说明

- 全部 10 个 Phase 的实现/测试/文档已入库并推送;板上联调仅剩"硬件确认"
  相关项(引脚/协议/固件),代码侧无阻塞
- 常用入口:door-guard/README.md(模块索引)/ modules/access/README.md(走查步骤)/
  docs/tech/OTA_PLAN.md(A/B 方案)

---

## 2026-09-18 Phase 9 网络功能完成

### 完成内容
- **web 上位机**(civetweb 1.16 vendored,NO_SSL+USE_WEBSOCKET):
  单页蓝白 UI(登录/实时事件/日志查询/设备管理/视频占位);
  登录 token(PBKDF2 凭据 device_config,默认 admin/admin);
  WS 实时推送、日志查询(与 db 直查一致)、OTA 上传端点
- **OTA 应用侧**:流式收包+大小预检+sha256 校验+续传;闭环到暂存文件
  不刷分区;OTA_PLAN.md(应用级 A/B + uboot env 约定,方案文档)
- **ntp_service**:三触发点+联网探测+chronyc;**mdns_responder**:
  doorguard.local A 记录应答(轻量自实现)
- tests/web:web_test.sh 14 项全过(401/200/400/422 断言)、ws_test.py、
  mdns_test.sh;dg-ota-upload 脚本
- **UI 事件队列(ui_events)**:总线线程禁止直接调 LVGL(实 crash 教训),
  总线回调入队、LVGL 100ms 泵出——所有页面已切换此模式

### 坑
- civetweb 编译宏:inl 文件需 src 目录 include;NO_SSL 下 websocket 握手
  的 SHA1 需 OPENSSL_API_3_0=1(跳过 openssl SHA_CTX 兼容路径)
- LVGL 非线程安全:web/总线线程直接调 lv_* 会堆损坏崩溃 → 事件队列强制
  LVGL 单线程访问(所有 UI 总线回调只入队)
- WS 推送改"客户端 ping 触发排水":跨线程 mg_websocket_write 在客户端
  异常断开时崩溃(实测),改由 civetweb 自有线程写

### 遗留(记录不阻塞)
- WS 客户端异常断开场景偶发崩溃:已改轮询排水规避,压力场景待压测
- 板失联中(Phase 8 gpio 事故):web/NTP/mDNS 板上实测待板恢复
- mDNS WSL2 NAT 组播受限:/etc/hosts 兜底方案已写入 mdns README

### 未完成 / 下一步
- Phase 10 集成与文档收尾(总验收清单自测)

---

## 2026-09-18 Phase 8 板上 HAL 完成 + 板失联事故

### 完成内容
- **gpio_hal**:B4 rootfs 无 libgpiod/gpiod CLI(实测)→ sysfs 实现;
  引脚号入 device.json(access.relay_gpio_line);开门脉冲+电平读回对拍接口
- **uart_hal**:termios 框架(原始模式+接收线程)+ mock 回环后端;
  test_uart_mock 回环三帧/重复打开拒绝/关闭后拒发全绿;
  **协议纪律执行**:指纹/读卡帧协议等手册,auth/{finger,card} 层未动
- **camera_board**:实测探测到 /dev/video0-72(rkisp 多节点)——B6/B7
  ISP 链路定位的重要线索;真实取流仍 mock 占位
- access_service OPEN_DOOR 接 gpio_hal(初始化失败降级为仅事件可观测)

### ⚠️ 事故记录(引以为戒)
- 板上 gpio 对拍时选择了未知引脚 gpio108,写 direction 后**板上内核挂起
  失联**(ping 不通),需**物理断电恢复**;door-guard 程序本身无恙
- 教训:GPIO 物理操作必须先确认引脚复用状态(pinctrl),未知引脚禁止写
  direction。gpio_hal 代码路径正确性待引脚确认后重测

### 待硬件确认清单(累计)
1. 继电器接线引脚(access.relay_gpio_line)与继电器类型(电平/脉冲)
2. 指纹模块型号/协议/接线 UART
3. IC 读卡器型号/协议/接线
4. 摄像头真实链路:rkaiq 3A + V4L2(B6/B7,/dev/video0-72 已探明)
5. 触摸:GT9xx 待 B5 固件(B4 FTS probe fail 无输入节点)

### 未完成 / 下一步
- Phase 9 网络功能(web/OTA/NTP/mDNS)

---

## 2026-09-18 Phase 7 服务层完成

### 完成内容
- **access_service**:FSM 唯一持有者;日志落库与 EV_AUTH_RESULT 唯一出口;门控事件;
  UID 解析/密码验证(连错预检)服务侧完成;FSM 定时器经 tasker 回注总线(单线程语义)
- **vision_service**:特征槽位句柄模式(8 槽环形,明文即取即清,大数据不过总线);
  sim mock(周期事件+确定性伪特征)+ rockiva 占位(B7/B8)
- **enroll_service**:录入编排(请求→抓取→查重→入库→回执);**capture_service**
  相机状态巡检广播;**liveness_service** 占位
- events.h 扩展 UI↔服务契约(EV_UI_BTN/TEXT_INPUT/METHOD_PICK/TOUCH/GOTO_PAGE/HINT);
  page_home 瘦身为纯渲染+事件转发
- test_e2e:事件总线全链路——录入→查重→入库→可命中;命中→开门+日志 result=0;
  陌生人 1.5s→失败 reason=1;密码错/对两路径;14/14 常规+tsan 全绿,三端零警告

### 坑
- tsan 连抓两处:FSM 心跳在 tasker 线程直接驱动(改经总线回注);
  event_bus `initialized` 普通 bool 与在途 publish 竞争(改 C11 原子)
- enroll 的 db_user_update 覆盖语义会清 role/auth_flags——编排侧先取旧记录回填
  (教训:覆盖式 update 调用方必须带全量字段)

### 未完成 / 下一步
- Phase 8 板上 HAL(gpio_hal/uart_hal 框架/camera 板上链路)

---

## 2026-09-18 Phase 6 三页面 + 验证状态机完成

### 完成内容
- **auth_fsm**:纯 C 事件驱动状态机,事件注入+动作回调,不碰 LVGL/DB;
  timer_seq/密码连错锁定/黑名单/日志唯一出口逐条实现;test_auth_fsm 11 用例
  40+ 断言覆盖 spec-auth-business §5 全部 10 组边界
- **七页面**:home(推流 canvas+黄/绿/红脸框+菜单验证按钮+四类弹窗)/
  standby(黑屏 HH:MM)/menu(四宫格)/users/device/access_set/logs,
  语言切换经 EVENT_UI_REFRESH_REQUEST 全页重建刷新
- **板上显示打通**:rockchipdrmfb(/dev/fb0)mmap 返回 EBUSY(实测,仿真层
  限制)→ 改走 lv_drivers DRM dumb-buffer(libdrm);开机 lv_demo 占用 fb
  需停用(已 mv disabled);板上主页渲染截图 docs/img/board-home-phase6.png
- **模拟器**:七页面全部渲染验证;DG_SIM_VISION=0 可关视觉 mock 交互走查;
  全流程截图(输入弹窗/键盘/成功失败弹窗/待机)

### 待确认(硬件)
- 板上触摸:B4 固件无 GT9xx/FTS 输入节点(FTS probe fail),待 B5 固件;
  板上 UI 当前只显示无触摸
- 字体注记:DroidSansFallbackFull 无 ASCII 字形 → 字体生成脚本改双字体
  (DejaVu Latin + Droid CJK),gen.sh 已固化

### 坑
- `source env/env.sh` 必须在仓库根执行,子目录静默失败导致几轮"板上没跑
  新二进制"的假象(md5 校验才定位到)
- DroidSansFallback 无 Latin → 数字全方块;lv_font_conv 多 --font 段解决
- 板上 lv_demo 占用 fb/DRM,door-guard 启动前须停(已禁自启,待 B10 rootfs 收编)

### 未完成 / 下一步
- Phase 7 服务层(access/enroll/vision/capture/liveness,全接 event_bus)

---

## 2026-09-18 Phase 4 配置体系 + Phase 5 UI 框架/PC 模拟器完成

### 完成内容
- **Phase 4**:cJSON vendored(third_party/cjson,MIT);config/cfg 三层覆盖
  (默认→device.json→DB device_config),类型错/越界 WARN 回退不崩;cfg_set 校验+
  持久化;test_cfg 全绿;device.json 补齐任务要求业务键
- **Phase 5**:LVGL 8.3 vendored 双端同源编译(自定义 lv_conf:CJK 字体+256KB 池);
  theme token / i18n(_()+双 json+常驻表缓存)/ widgets(dg_btn/dg_popup×4/dg_kbd/
  dg_list)/ page_mgr / 自定义中文字体(gen.sh 从 lang 字符集生成);
  hal/display sim=SDL2 自写驱动 720×1280,hal/camera sim=stb_image 图片循环;
  dg-build-pc 脚本;test_i18n(键覆盖+裸中文=0+字形覆盖)+ test_widgets(无头冒烟)
- **验收达成**:模拟器 720×1280 稳定运行,截图 docs/img/sim-phase5-widgets.png;
  dg-build 交叉零警告;dg-test 12/12(常规+tsan)全绿;裸色值=0、裸中文 label=0(自动化)

### 踩坑记录
- **LVGL lv_conf.h 模板整体包在 `#if 0`**:忘改 `#if 1` 导致全部配置不生效
  (字体缺符号/定时器异常),pragma message 是线索
- **include 守卫与 token 同名**:theme.h 的 `#define DG_BTN_H 96`(按钮高度)撞上
  dg_btn.h 的守卫 DG_BTN_H,头文件被整体跳过 → 隐式声明 → 指针截断段错。
  守卫一律加模块前缀(DG_WIDGETS_BTN_H)
- **静态库符号环**:lv_conf LV_TICK_CUSTOM 回引 ui/port.c 而部分 lvgl 成员被 ui 引用,
  单遍 ld 解析不了 → 应用链接用 `-Wl,--start-group/--end-group`
- **UTF-8 三字节解码掩码**:第二字节是 0x3F 不是 0x1F(复制 2 字节分支的笔误)
- tests 配置分支若在目标定义前 return,后面的库对测试不可见——分支必须放最后
- 字体策略:内置 SIMSUN_16_CJK 是日文/繁体字集(简体几乎全缺),必须自生成;
  宿主已有 node(lv_font_conv),字体 .c 入库使测试机免 node

### 未完成 / 下一步
- Phase 6 三页面 + 验证状态机(spec-auth-business §5 边界 ≥15 条用例)

---

## 2026-09-18 Phase 2 proto 层 + Phase 3 storage 完成

### 完成内容
- **Phase 2**:`proto/{err.h,types.h,events.h}` 统一契约。错误码分段(通用/用户/验证/网络);
  user_rec_t/access_log_t/log_query_t 字段对齐 spec-database,role/auth_flags 位宽
  _Static_assert;18 种 EV_* 事件+负载(编译期 ≤256B 守卫)+ 事件契约测试(发布→订阅
  回捕逐字段相等 + EV_AUTH_RESULT 显式比对)。proto/README 含使用示例
- **Phase 3**:`hal/storage`(storage.c/crypto.c)。DDL 与 spec 逐字一致;加密走 sysroot
  openssl3 EVP(PBKDF2-HMAC-SHA256 10000 iters / AES-256-CTR 随机 IV 前缀+设备密钥 dg.key);
  添加全链路校验(密码必填/uid/IC/特征查重/2000 上限);特征查重用比较器注入
  (`storage_set_feature_cmp`,基线逐字节,Phase 7 注入 ROCKIVA 相似度);日志查询
  时间段含边界/按用户/分页/倒序;配置 KV upsert;特征迭代器供 enroll 编排
- 验收:sqlite3 CLI 校验 schema 一致;库文件/密钥 0600;dg-test 9/9(常规+tsan)全绿;
  dg-build 零警告;宿主 apt 装 libsqlite3-dev(测试构建用,记 DEV_HANDBOOK)

### 踩坑记录
- **LIMIT ?N 动态占位符错位**:`LIMIT ?4` 在无 WHERE 时 ?4 未绑定 → SQLite 视为 NULL
  → 0 行返回。改为已校验整数内联 LIMIT/OFFSET,过滤值保持绑定
- **测试数据算术**:seed 里 i%50==0 的行同时 i%5==0(陌生人→user_id NULL),"按 U000
  查 50 条"永远查不到;同理 i=4 不是陌生人。教训:测试数据生成器要和断言一起推演
- tsan 连抓三处测试代码竞争(DG_CHECK 全局计数被 handler 线程改)——测试代码也要原子纪律

### 未完成 / 下一步
- Phase 4 配置体系(configs/device.json 全参数化 + cjson 加载器)

---

## 2026-09-18 Phase 1 基础组件移植完成(event_bus → tasker → holder)

### 完成内容
- `proto/dg_log`:组件共用极简日志(承接模板 logger,后续统一日志模块只换实现)
- `proto/event_bus`:pthread port;锁外回调/事件池+堆兜底/原子统计保留;新增
  dg_event_pool 自实现定长块池、分发任务 stop+join 清理路径;模板测试 EB1~EB5
  断言未弱化;新增 4×10000 压测(载荷 (tid,seq) 恰好一次 = 零丢失)+ tsan 全绿
- `proto/tasker`:pthread port;5.2 三修复与自旋熔断逐行保留;**tsan 检出模板固有
  数据竞争**(跨线程标志非原子 + is_empty/is_full 无锁读),统一改 C11 原子访问 +
  补调度表锁,调度逻辑不变,TSAN 复跑全绿
- `proto/holder`:pthread timedlock 等价带超时取锁;新增 test_holder 61 断言
  (循环依赖/重复注册/ERROR 态隔离/必需模块停机)
- `dg-test` 脚本建成(宿主 gcc + ctest,`--tsan` 可选);7/7 常规全绿、tsan 全绿;
  dg-build 交叉编译零警告;demo×3 全部可执行并进 ctest 冒烟

### 结论 / 坑
- **压测 drop 计数语义**:event_bus 的 `events_dropped` 是"队列满丢弃的发布尝试
  次数",发布方按契约重试后事件不丢;零丢失须由载荷序号恰达一次证明,不能断言 drop==0
- 模板质量总体高,但"5.2 修复版"在 tsan 下仍有竞争——移植不是复制,并发组件必须跑 tsan

### 未完成 / 下一步
- Phase 2 proto 层(err.h/events.h/types.h + 静态断言 + 事件契约测试)

---

## 2026-09-18(闲时任务开工)10 Phase 应用开发启动

### 完成内容
- 按开工三步恢复上下文:git log 确认全部 Phase 未开始,从 Phase 0 起步
- 通读 skill references 全部五份(spec-database / spec-auth-business / spec-ui / spec-network / architecture)
- Phase 0 基线自检:`source env/env.sh && dg-build -c` 成功,git status 干净

### 本次计划 Phase 顺序
- Phase 1 基础组件移植(event_bus → tasker → holder)
- Phase 2 proto 层 → Phase 3 storage → Phase 4 配置体系
- Phase 5 UI 框架 + PC 模拟器 → Phase 6 三页面 + 验证状态机
- Phase 7 服务层 → Phase 8 板上 HAL → Phase 9 网络功能 → Phase 10 集成收尾
- 每 Phase 测试与验收全过才进下一 Phase;卡点 >30min 绕行并记 DEVLOG

---

## 2026-09-17(晚)目录整理 + door-guard-dev skill + 环境资产落库

### 完成内容
- 目录重构:`docs/`(DEVLOG/TECH 文档)、`env/`(env.sh + dg-build/dg-deploy/dg-tc-install/dg-serial,source 后免路径);FLASH_GUIDE 迁至 docs/tech/FLASHING 并修正 IDB/分区表错误(parameter 0x800→0x0)
- WSL 工具链:sysroot 坏包已由 VM 重导(软链未解引用),sqlite 探针通过;cmake 工具链文件适配真实目录名+多用户探测
- 创建 `.agents/skills/door-guard-dev/`:SKILL.md 工作流 + 5 份 references(数据库/认证业务/UI/网络/架构),全量业务规格入库
- ESP32 模板(/home/olwhistle/dockerNow/esp32/programs/ovs)API 已确认(tasker/event_bus/holder,带 port 层),映射表写入 skill references/architecture.md
- 环境资产落库:板 IP 192.168.2.95(env.sh 默认)、root 口令、Gitea 地址、模板路径 → DEV_HANDBOOK §4/§6
- 板子已装 WSL 公钥免密;`dg-deploy -r` 零参数推板运行验证通过

### 未完成 / 下一步
- 移植模板组件:event_bus → tasker → holder(按 skill architecture.md §2.2 纪律)
- 主页面验证状态机、用户管理页、OTA 分区方案(对照 references 规格)
- WiFi 驱动修复、recovery、ISP 节点定位(B6)仍挂账

---

## 2026-09-17 首版固件产出 + 烧录上板 + WSL 交叉编译链路打通

### 固件(B1~B4,VM 侧)

- SDK 解压(19GB)并通过 `git fsck --full` 全量校验,建 `k7-door-guard-dev` 分支
- Buildroot doorGuard 定制配置(无桌面,LVGL+DRM、ROCKIVA、RKADK+RKAIQ、rknpu2、
  GStreamer+RTSP、SQLite、dropbear/chrony),5 寸屏 F050008M01 使能
- 产出 **20260917-B4** 首版固件(分区五件套),后补 `update.img` 一键包(488MB,recovery 空)
- **已知缺口**:WiFi(SWT6621S)驱动编译失败(厂家脚本头文件拷贝顺序 bug),联网用以太网;recovery 延后

### 烧录(坑:IDB 保留区)

- **坑**:分区烧录时按老平台习惯把 parameter.txt 填 0x800 扇区,被 RKDevTool 拦截:
  "IDB 将会被 parameter.txt 破坏,不要写数据在 4824 扇区之前"
- **结论**:RK3576 的 eMMC 前 8MB(0x4000 扇区前)是 BootROM/Loader 保留区;
  Loader 与 parameter 两行都填 **0x0**(工具特殊处理),其余分区镜像 ≥0x4000
- 实际采用 update.img 一键烧录成功,Loader 刷坏也可走 Maskrom 救回(不依赖 eMMC)
- 详见 `docs/tech/FLASHING.md`

### B5 验收进展

- ✅ 串口登录(1500000 8N1)、屏幕点亮 + GT9xx 触摸、IMX415 出流(cam2,3864×2192 RAW10)
- ⏳ ISP 节点定位 / NV12 抓帧(B6)、rknpu 驱动确认
- **坑**:以太网开机不自动配置——rootfs 装了 dhcpcd 但没有开机脚本拉起,`eth0/eth1`
  停在 `state DOWN`(管理关闭,不是硬件问题);手动 `ip link set eth0 up` + `dhcpcd eth0` 即通
- ✅ dropbear SSH 可登录(root;dropbear 拒绝空密码,先串口 `passwd root`)

### WSL 交叉编译环境

- 从仓库 `deliverables/wsl-toolchain/` 安装:gcc-arm-10.3(aarch64-none-linux-gnu)+
  doorguard sysroot(B4 buildroot staging,glibc 2.38,与板上 .so 严格同源)
- **坑**:sysroot 首次打包误把 staging **符号链接**直接打进 tar(276 字节坏包),
  必须解引用(`tar czfh`)或 `-C` 进实体目录归档;已重导修复,命令见 `docs/tech/TOOLCHAIN.md`
- **坑**:gcc 包解压后目录名带版本号(`gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu`),
  与文档不符;`cmake/aarch64.cmake` 已改为自动探测 + sysroot 三级回退
- sqlite3 探针编译链接通过(NEEDED libsqlite3.so.0,B4 rootfs 自带)
- ✅ **冒烟程序板上运行通过**:`arch=aarch64 kernel=6.1.75`,"WSL 写码 → dg-build → dg-deploy → 板上跑"全链路打通

### 下一步

1. 开机自动配网(脚本化 dhcpcd 自启,进 buildroot overlay,下一版固件带上)
2. B6:ISP 节点定位 + rkaiq 3A + NV12 抓帧;B7:ROCKIVA 上板
3. WiFi 驱动修复、recovery 补齐(不阻塞)
