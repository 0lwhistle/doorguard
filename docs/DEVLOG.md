# 开发日志(DEVLOG)

> 记录约定:每次会话/每个工作日**追加**新条目(最新在最上),写清"做了什么 / 结论 / 踩了什么坑"。
> 本日志记"过程与坑",当前状态看 `DEV_HANDBOOK.md`,方案看 `PROJECT_PLAN.md`。

---

## 2026-09-21 文档同步:主线切自组 rknn 后的全量对账

**做了什么**(代码未动,纯文档;核验通过后提交)
- **PROJECT_PLAN**(唯一事实来源):①进度快照整行重写(自组 rknn 主线上板跑通、
  ROCKIVA 搁置原因、NPU 库落位、UI 重做、28/28 绿、遗留清单);②§3.2 数据流加
  识别支路(ROI→对齐→ArcFace);③§3.4 目录补 `drv/npu`、`models/`、tools 三件套、
  测试数 22→28;④**§4.3 整节重写**:标题从"官方 ROCKIVA 方案(已定)"改为
  "自组 rknn 方案(主线,2026-09-21 切换;ROCKIVA 备选)",含两模型实测表与两条
  "无报错但全错"的坑、阈值标定遗留。
- **`services/vision/README.md`**:后端清单从"两个 + 将来"改为**三实现对照表**
  (rknn 主线/rockiva 备选/sim),新增「rknn 链路的四层分层与"在哪测"」表 +
  两条血的教训;配置键补 rknn 三个 env;缺口清单补质量闸门与阈值标定。
- **`docs/DEV_HANDBOOK.md`**:速览改人脸主线;硬件事实表补 NPU 口径(int8 才是
  6TOPS 口径)、相机 **stride=1280 实测**、板上**无 python3**;§4 板端速查补整段
  rknn 命令(探针/两个对拍工具/日志 grep/特征口径 SQL)+ 板端路径备忘。
- **`docs/tech/B7_FACE_HANDOFF.md`**:顶部加**路线变更警告**(ROCKIVA 降备选,
  指向新文档);§2.4 末补"本版 SDK 无人脸模型"的排查结论与厂商渠道建议。
- **`docs/architecture-v2-proposal.md` §11**:测试数 25→28 并列出新增三项;
  ②目录形态补 drv/npu 已填与 vision 三后端;⑤文档同步态改写;**①提交链自查
  从"硬编码 6 个哈希"改为按形态核验**——哈希每次提交都会烂,形态核验才耐久。
- **skill**(`.agents/skills/door-guard-dev/SKILL.md`):文档索引表补 3 行
  (vision/models/drv-npu 三份 README);环境速查补"人脸=自组 rknn 主线"事实。

**核验**:文档引用的 7 个文档 + 11 个代码文件全部存在;测试数声明与 `ctest -N`
实际(28)一致。

---

## 2026-09-21 编辑页三缺陷修复(用户反馈)+ 录入链路端到端测试(test_enroll_flow)

**根因(用户反馈"编辑页显示无/提交不了/录入没反应")**
- **行点击拿到 NULL**:LVGL 里 `lv_event_get_user_data(e)` 返回**回调注册时**的
  user_data(dg_list 行为 NULL),不是 `lv_obj_set_user_data` 设的 uid——编辑页
  open() 拿空 uid → db_user_get 失败 → **误判为添加模式**(全部显示"无"、
  face 按钮隐藏、保存时 ID 为空必败)。旧版用户管理的行点击同样中招。
  修:改用 `lv_obj_get_user_data(lv_event_get_target(e))`(键盘组件本来就是
  正确用法,只有列表行错)。
- **清除人脸清不掉**:`db_user_update` 语义是 len=0=保留(部分更新模式),
  无法表达清除 → 新增 `db_user_clear_face()`(置 NULL+特征缓存增量同步),
  enroll 的 FACE_CLEAR 改走它——**测试抓出**,与用户"数据显示无"是两回事,
  同日双修。
- **录入无响应**:链路任何一环没回执(3s 内无人脸/后端异常)UI 永远无声。
  修:编辑页加 5s 超时定时器(有回执即撤),超时弹"请正对摄像头重试";
  后端 on_capture_req 加成功日志(长度+滞后 ms)便于板上定位。

**测试(用户要求自验)**
- 新增 `tests/test_enroll_flow.c`(28 项中的端到端):**sim mock 后端应答抓取**,
  请求→回执→DB 状态一致,覆盖 录入/重录/清除/对不存在用户(失败回执)/删除。
  首跑即抓出 db_user_update 清不掉人脸的缺陷——链路测试的价值实证。
- 全量 **28/28 ctest 绿**(27+enroll_flow),交叉编译零告警,已推板。

**遗留**
- 质量闸门(检测分+最小脸尺寸+清晰度)与阈值标定(1:N 最高分日志为依据)。

---

## 2026-09-21 UI 四项用户反馈:脸框防闪/用户编辑页/按钮去图标/弹窗补取消

**做了什么(27/27 测试绿,交叉零告警,已推板)**
- **脸框时有时无**:`vision_rknn.c` LOST 加 600ms 滞回——单帧漏检(分数抖动/识别帧
  占用)不再立刻撤框,超时才判"人走了"。根因:识别帧占用 NV12 缓冲造成检测间隙,
  边沿触发把间隙放大成闪烁。
- **用户编辑页**(新 `page_user_edit.c` + presenter):添加/编辑**同一模板**,一页
  看全 用户ID/姓名/权限/密码/人脸/指纹/IC卡,没有的显示"无"。EDIT 即时落库;
  ADD 先攒姓名/密码,[保存] 才建用户(必设密码硬规则),建好自动转编辑模式。
  人脸=录入/重录/清除;指纹/IC 点击提示"硬件未接入"(不静默)。打开时按
  **用户是否存在自判模式**,列表页只需传 ID。
- **`page_users.c` 重做**:只留列表+入口(行点击→编辑页;添加→输 ID→编辑页),
  旧的三步入库/行内菜单删除;顺手修 `err_text` 把 DUP_UID 映射成"该卡已绑定"的错误。
- **按钮去图标**:键盘的 删除/确认 改文字、"Aa" 大小写键;page_users/web_set 按钮
  与列表行全部去 `LV_SYMBOL_*`(CN 字体无 FontAwesome 码位,渲染不出)。
- **弹窗补取消**:`dg_popup_choice` 的 on_cancel 回调此前**存了但没有 UI 入口**——
  补取消按钮;输入弹窗取消按钮去图标。结果弹窗(自动关)不变。
- **录入闭环**:`EV_ENROLL_RESULT` 经 bridge 转发进 UI(UI_EVT_ENROLL_RESULT),
  编辑页按回执弹成功/失败并刷新;新增 `DG_ENROLL_FACE_CLEAR`(清除人脸,经 enroll
  服务保证视觉特征库同步)。
- **i18n**:新增 24 键(中英),`ui/font/gen.sh` 重生成字体(修 npx 把
  symbols 首字符"—"当参数的问题:前置空格);test_i18n 的"裸中文"扫描连注释里的
  ASCII 引号都查——注释引号统一中文引号。

**踩坑**
- `LV_FONT_MONTSERRAT_28/48` 是启用的,按钮图标不显示的真正原因是**经过 CN 字体
  渲染的 label**(列表行/键盘键位)拿不到符号字形;与其查哪个路径漏,不如按用户
  要求全面文字化。
- 新增中文字符必须先进 lang JSON 再跑 gen.sh,否则字体缺字形(有 test_i18n 兜底)。

**下一步**:板上验收编辑页与 1:N 命中;质量闸门与阈值标定(遗留)。

---

## 2026-09-21 识别链路接通:ArcFace 输入约定实测(必须 f32 归一化)+ 完整 1:N 后端上板

**ArcFace 输入约定的实测(本日最重要的结论)**
- `w600k_r50.rknn` **没有烤入归一化**(与 RetinaFace 不同,后者 mean/std 烤进图)。
  用"同人正常/变暗/纯色画布"三张 112×112 在板上对拍:
  - u8 直喂:cos(脸,纯色)=**0.79** ← 全部 embedding 高度相似,识别永不命中且**无任何报错**;
  - **f32 按 (x-127.5)/127.5 预归一化:cos(同人,变暗)=0.984、cos(脸,纯色)=0.106 ✓**。
  结论:识别路径必须 `rknn_rgb_norm_f32()` 预归一化后喂 F32。这个坑没有任何错误日志,
  只有对拍能暴露——验证先行少走了整段弯路。

**做了什么**
- **`proto/types.h`:`DG_FEATURE_MAX` 512→2048 B**(ArcFace 512 维 float32;
  users.features 是 BLOB 免迁移,2000 人特征缓存 ≈4 MB)。
- **`vision_rknn.c` 完整识别**:每 300ms(非 IDLE)从 **NV12 原分辨率** RGA 裁人脸 ROI
  (`npu_pre_nv12_crop_rgb`,偶对齐;不在 320 画布上对齐——那等于放大 4 倍喂识别)→
  5 点相似变换 112×112 → 归一化 → ArcFace → L2 → 录入缓存 / 1:1 / 1:N。
  特征库=内存数组(启动 `db_user_iter_face` 全量装载,lib_add/lib_del 维护,
  检索=2000×512 余弦暴力,毫秒级);compare=余弦≥face_dup_threshold;
  黑名单/口径/活体三重门禁照 ROCKIVA 后端语义。
- `npu_model_run` 尺寸守卫修正:**期望缓冲尺寸跟随声明的输入类型**算
  (U8 给 F16 模型是每元素 1 字节,原先错按张量尺寸拒绝自己合法的输入)。
- 板上 `face_model_tag` 已更新为 `rknn-arcface-r50-v1`(**在 DB
  /var/lib/door-guard/door-guard.db 的 device_config 表**,不在 cur_config.json——
  它是 DB 冻结的遗留键,不在 JSON 键集内);重启后干净启动,屏蔽解除。
- 测试 54 项(+5:归一化),全量 27/27 ctest 绿,交叉编译零告警。

**当前板上状态**:`rknn 就绪:检测+识别(512 维,特征 2048 B),库 0 人`,
vision_backend READY。**待用户操作**:UI 录入一张人脸 → 主页刷脸 → 绿框+开门
(1:N 命中;阈值 0.42 是起点,按 2s 节流日志"1:N 最高分"实测标定)。

**遗留(有意未做)**
- 质量闸门未上(检测分+最小脸尺寸+清晰度)——识别通了之后加,防模糊脸误录/误判;
- 查重阈值 0.90 是 ROCKIVA 分度,余弦分度需标定(重复录入可能查不出重);
- ROI 裁剪坐标偶对齐用 `&~1`,face 贴帧边时识别会跳过该帧(下一帧自然恢复)。

---

## 2026-09-21 自组 rknn 检测链路打通并上板(RetinaFace 重转 6.7ms;解码对拍通过)

**做了什么(第三批:后端装配)**
- **`services/vision/vision_rknn.c`**:rknn 后端(契约 vision_backend.h)。链路全部
  在相机线程内联完成(检测 6.7ms + RGA 亚毫秒 ≪ 33ms 帧预算,且用完立刻归还
  V4L2 缓冲,不新增线程也就不会饿死 4 缓冲):
  camera NV12 → RGA letterbox 320×320(补 114)→ RetinaFace@NPU(喂 U8)→
  `rknn_retinaface_decode` → NMS → 最大脸 → 逆 letterbox + 旋到竖屏 →
  `EV_VISION_FACE_BOX`(10Hz 节流)/ `EV_VISION_FACE_LOST`(边沿)。
  5 点关键点回灌 `liveness_service_on_face`(B8 几何活体用的正是 5 点)。
  启动时**自校**:输出数须为 3、锚框数须等于解码器按输入尺寸的期望值,
  不符即启动失败(比每帧给错框好定位)。
- `app/main.c` 注册顺序 = 优先级:rknn 在前(缺省选中),ROCKIVA 保留备用。
- 新工具 `tools/rknn_det_test.c`:喂一张已 letterbox 好的原始 RGB 走完整
  "推理+解码",用于**无人站镜头前**的对拍。
- 板上实测启动日志:`rknn 就绪:RetinaFace 320x320,锚框 4200,检出阈值 0.50`
  → `service vision_backend READY`;letterbox 计划 `1280×720→320×320
  scale=0.2500 补边 0,70`(与宿主单测预期一致)。

**解码对拍(关键验证)**:拿 zoo 的 `test.jpg`(已知人脸位置)按同法 letterbox 成
320×320 原始 RGB,喂进板上完整 C 链路:

```
检出 1 张脸 (152,90)-(240,203) 87x113 分数 0.9990
关键点 (178,138)(219,140)(198,162)(182,181)(212,183)
```

独立 Haar 参考脸换算到模型空间是 (144,101)-(248,205),**中心几乎重合**
(X 中心 196 vs 196.5);关键点解剖学正确(双眼同高、鼻居中、嘴角在下)。
→ 输入假设、推理、解码、NMS 全链路正确。单张图 7.6ms。

**踩坑 / 现状**
- rknn 的 x86 模拟器起不来(`smartsocket listener: Address already in use`),
  所以参考基准改用 onnxruntime/Haar 交叉验证 + 板上对拍,不依赖模拟器。
- 板上会打一条**预期的** ERROR:`人脸特征口径不一致:库=rockiva-face-v1
  当前=rknn-arcface-r50-v1`——device_config 里留着上次开机登记的 ROCKIVA tag。
  这是设计行为(换模型空间必须重录),不是故障;录入人脸后按提示改 tag 即消。
- **唯一未验证点**:RGA 输出是 RGB 还是 BGR 字节序(对拍用的是 python 备好的
  RGB,绕过了 RGA)。若上板看不到框,先改 `npu_pre.c` 的 `RK_FORMAT_RGB_888`
  → `RK_FORMAT_BGR_888` 试(一个常量)。相机 stride 已从日志确认为 1280(= 宽),
  紧凑排布假设成立。

**下一步**
- 站镜头前验收检测框(黄框跟随);随后接识别:112×112 对齐 → ArcFace 512 维 →
  余弦比对 → 1:N(复用 M2 特征内存快照),同时 `DG_FEATURE_MAX` 512→2048B、
  启用 lib_add/lib_del/compare/on_mode;再接质量闸门与 B8 活体。

---

## 2026-09-21 自组 rknn 路线开工:NPU 推理库 + RetinaFace 重转成功 + 解码单元(49 项宿主测试绿)

**背景**:ROCKIVA 官方 rk3576 人脸模型包在这版 SDK 快照里缺失(external 与 buildroot
两处、iva.tar 内均只有前级检测 `object_detection_v3_cls8.data`;rk3588/rv1126 目录才有
人脸件)。官方模型须走 Kickpi 厂商渠道,不阻塞——改走用户拍板的自组 rknn 路线。

**做了什么**
- **建库 `drv/npu/`**(架构里预留的空位):`npu_model.c/h` = RKNN 运行时薄封装
  (加载/查张量/喂输入/推理/取输出/释放),**全仓唯一 include `<rknn_api.h>`** 的文件;
  不认识任何具体模型,模型专属后处理留在 services/vision(与 drv/gpio 同纪律)。
  输入尺寸不写死——加载后查出来,换模型不必改代码。附 README(定位/接口/坑)。
- **`tools/npu_probe.c`**:模型探针,打印真实张量规格 + 零输入试跑 + 压测
  (`DG_NPU_BENCH=N`)。换模型第一件事,避免猜输入尺寸。
- CMake:`dg_npu` + `npu_probe`(`NOT DG_SIM AND NOT DG_BUILD_TESTS` 守卫,宿主无 rknn);
  交叉编译零告警。

**板上实测结论(记进 `models/README.md`)**
- **`RetinaFace.rknn` 不可用**:驱动直报 `This rknn model is for RK3588, but current
  platform is RK3576`——转换时目标平台选错。要用须按 RK3576 重转。
- **`det_10g.rknn`(SCRFD-10G)可用**:输入 640×640×3 NHWC F16,9 输出 = 3 stride ×
  (score/bbox/kps)、每位置 2 anchor;顺序 = `[s8,s16,s32,b8,b16,b32,k8,k16,k32]`。
- **`w600k_r50.rknn` 可用**:输入 112×112×3 NHWC F16,输出 `[1,512]`=512 维
  → 证实 `DG_FEATURE_MAX` 须 512→2048 B 才能装下。
- **稳态耗时**(压测 30 次):检测 177.6ms(≈6fps)、识别 55.6ms(≈18fps),
  一次「检测+识别」≈240ms。门禁站定刷脸可用,框跟踪不顺滑;提速杠杆(int8 量化重转 >
  降输入分辨率 > 每 N 帧检测)已记进 models/README。

**踩坑**
- 探针输出缓冲写死 4096 → SCRFD 的 12800 元素 score 头被 `npu_model_output_f32`
  正确拒掉(不截断,报 DG_ERR_PARAM)——库的行为对,是探针该按 attr 分配。
- `snprintf` 拼两个 256B 版本串触发 `-Wformat-truncation`;定长 `%.255s` + 放大缓冲消除。

**RetinaFace 重转成功(本机 rknn-toolkit2 2.3.2,用户提供 zoo + toolkit)**
- `RetinaFace.rknn` 是 RK3588 模型(板上驱动拒收)。用 zoo 的
  `examples/RetinaFace/model/RetinaFace_mobile320.onnx` 按 **rk3576 + i8** 重转,
  标定集用 zoo 的 COCO 20 张子集 + 例程 test.jpg(单张标不准量化范围)。
- 新文件 `RetinaFace_rk3576_i8.rknn`(1.4MB):输入 320×320×3 **I8**、
  3 输出 `[1,4200,4]/[1,4200,2]/[1,4200,10]`;**压测均值 6.7ms、最快 5.8ms**。
- **比 SCRFD 快 30 倍**(178ms→6.7ms):320 vs 640 输入 + int8 量化 + 1.4MB vs 9.4MB。
  一次「检测+识别」≈66ms(15fps),框跟踪顺滑。RetinaFace 由"不可用"变首选。
- 关键便利:`convert.py` 已配 mean/std → **归一化烤进图**,运行时喂原始 uint8 RGB,
  不必自己写 F16 归一化。全流程记进 `models/README.md ⑤`。

**做了什么(第二批:后处理单元)**
- **`services/vision/rknn_face.c/h`**(纯 C 零依赖 → 宿主与板上都编):
  SCRFD 解码(3 stride/2 anchor)、**RetinaFace 解码**(PriorBox + variance,与 zoo
  参考实现逐条对齐;锚框数 320→4200 与板上实测互证)、NMS、5 点相似变换对齐
  (ArcFace 112×112 参考布局)、余弦/归一化。附 `tests/test_rknn_face.c` **49 项**:
  合成数据钉死 anchor 编号/stride 还原/0.5 偏移/阈值/截断上报、对齐用"参考点自映射
  =单位阵"自检、warp 越界填零。
- **修一个真 bug**:`npu_model_run` 原先按模型自带类型喂输入——**int8 与 uint8 缓冲
  字节数相同**,喂错不会报错只会静默出错图。改为显式声明 `in_type`
  (RetinaFace 喂 U8 由运行时量化、SCRFD 喂 F16),接口层面挡住这类静默错误。

**踩坑**
- 探针输出缓冲写死 4096 → SCRFD 的 12800 元素 score 头被 `npu_model_output_f32`
  正确拒掉(不截断,报 DG_ERR_PARAM)——库的行为对,是探针该按 attr 分配。
- `snprintf` 拼两个 256B 版本串触发 `-Wformat-truncation`;定长 `%.255s` + 放大缓冲消除。
- 测试里两处**我自己算错**:正交向量 {-4,3,0,0} 与 {1,2,3,4} 点积是 2 不是 0;
  size=0 该返回参数错(-1)而非 -2。宿主单测当场抓出——这正是把数学留在可测层的价值。
- `test_ota` 偶发失败(单跑必过),是测试自身时序抖动,非本轮改动(已复跑全绿 26/26)。

**下一步**
- `vision_rknn.c` 后端起:相机 NV12 → **RGA letterbox 320×320(补边 114)** →
  NPU(U8 输入)→ `rknn_retinaface_decode` → NMS → 质量闸门 → `EV_VISION_FACE_BOX`
  → 上板看框;随后接 ArcFace(112×112 对齐 → 512 维 → 余弦)与 1:N(复用 M2 特征快照)。
  `DG_FEATURE_MAX` 512→2048B 与独立 `face.model_tag` 在接识别时一并落。

---

## 2026-09-21 仓库对账:拉齐 GitHub + 清 M1 迁移残留(proposal §11 全项核验过)

**做了什么**
- 本地落后 origin 1 个提交(`1cc0013` §11 自查清单 + camera/display README 旧 include
  修复);工作树里未提交的 §11 与远端提交**逐字节重复**,restore 丢弃后 ff 拉取对齐。
- 删 M1 迁移残留目录 `modules/net/web/`(旧路径 node_modules + dist;dist 与现役
  `services/web/frontend/dist` 逐字节一致,依赖可按 package-lock `npm ci` 重建)。
- 修 M1 漏改的旧路径**活引用**:`tests/web/web_test.sh` 4 处(前端单测目录、
  build_frontend.sh 提示、pages 产物比对)、`services/web/README.md` 3 处
  (目录树根、cd 路径、vitest 路径)。

**核验(proposal §11 清单)**
- ② 目录形态 ✓;④ 红线 ✓(cfg.c 无 db_config_set / 无旧 include / models 只 README+sha256sums)
- ③ build-tests 重新 cmake 配置后 **25/25 全绿、构建 0 警告**。坑:旧构建目录只认 22 个
  测试,M2 新增的 3 个要重跑 `cmake .` 才纳入——**M 级提交新增测试后,旧构建目录须重配置**。

**踩坑**
- web_test.sh 的前端检查自 M1 起指向旧路径:若旧路径装过 node_modules 会"静默通过",
  残留目录一删才暴露。全库旧路径活引用已清零(DEVLOG 历史条目按约定不改)。

**下一步**
- B7 人脸模型联调;遗留项同上一条 2026-09-21(§9 注记)。

---

## 2026-09-21 架构 v2 M2 行为升级四项落地(25/25 测试绿)

**做了什么**(每项独立提交,WSL 全新构建回归后才提交)
- **M2① config 双文件**(`546833c`):cfg 重构为元表驱动;set/reset/迁移/加载四操作同源。
  default 模板(configs/default.json)+ 现用配置(板 /userdata/doorguard/cur_config.json,
  sim sim/data/)。cur 缺失 → DB device_config 已知业务键一次性导出,此后 DB 冻结
  (web 凭据留 DB,定位为凭据非配置——已知债务);cfg_set 内存生效+500ms 防抖;
  cfg_flush 原子落盘(tmp+fsync+rename);cfg_reset_key/all 按项/全部恢复默认。
  配置文件 NULL=无文件模式(测试)。
- **M2② 特征缓存**(`676bc07`):人脸特征启动全量装载、增删改增量同步(拷贝+原子切换);
  storage_features_ro()/ro_done() 只读快照(读写锁,持快照禁调其他 storage 接口——
  防锁序);维护失败自愈全量重载,再失败 broken→快照恒空(1:N 恒不命中,fail-closed)。
  新增 tests/test_feat_cache。
- **M2③ registry+看门狗**(`dc306a6`):components/registry(holder 同构+心跳/重启原语/
  依赖解析器注入/README);main.c 拆双表装配(holder=infra+modules,registry=services),
  初始化后 main 转看门狗 5s 巡检:可选服务异常→重启一次→仍异常 DISABLED+
  EV_SYS_SERVICE_STATE 通知;必需服务(vision_service/access)→安全停机
  (gpio_hal_set_level(0) 复位继电器+退出交 S60)。web 补心跳(推送线程唤醒刷新)。
  新增 tests/test_registry。
- **M2④ OTA 按需线程**(`2a8800e`):写线程流水线——web 线程只投递环形缓冲(256KB 背压),
  盘 I/O+摘要+终态校验/落位在按需线程(存在期=上传期,完成即退);断点续传重放移入
  写线程;**EV_NET_OTA_PROGRESS 首次真实发布**(≥5% 一拍+终态);DG_OTA_DIR 可覆盖
  暂存目录。新增 tests/test_ota(正常闭环/拒收/超限/BUSY/abort/续传)。

**踩坑**
- cfg_load 持锁调 cfg_flush → 非递归互斥自锁死锁(测试超时定位);抽 flush_locked 修复。
- tasker_cancel_by_name 在 tasker 未初始化时空锁段错误(头注称"自动初始化"未覆盖此
  API);flush_schedule 显式 tasker_init() 幂等兜底。
- 教训:grep -c warning 在日志未落盘时有竞态假象,零警告判定用 python 全字节扫描。

**没做完 / 遗留**
- DB 单写者请求队列未做(②缩小为特征缓存);EV_SYS_SERVICE_STATE 的 UI 提示渲染未接;
  心跳覆盖目前仅 web(其余服务状态监控);推送 GitHub 仍被 SSH 公钥阻塞。

**下一步**
- B7 人脸模型联调(模型已齐:ROCKIVA .data 待拷板 / 自组 rknn 三件已入 models/);
  遗留项随下轮;push 待公钥。

---

## 2026-09-20 架构 v2 决议 + M1 目录迁移落地

**做了什么**
- 用户拍板 4 项:只读直调按"高实时/高性能"放宽(登记制)/ device_config 表冻结 /
  RTSP 暂缓走 WS 快照 / 迁移立即。决议写回 proposal §1/§10。
- **M1 机械迁移**(全部 git mv 保历史,零行为变更):
  proto/{tasker,event_bus,holder}→components/、proto/dg_log→components/logger/;
  hal/{uart,gpio,npu}→drv/、hal/{camera,display}→modules/、hal/storage→modules/sqlite;
  modules/{capture,vision,liveness,access,enroll}→services/;
  modules/net/{web,ota,mdns,ntp}→services/(net_info 留守 modules/net);
  auth→services/verify(finger→fingerprint、card→ic);config→services/config。
- include 带路径引用 13 处 + 两份 CMake 全量替换;修 3 个迁移断点:
  ① dg_gpio/dg_uart/dg_display/dg_camera 原靠 dg_log 的 proto/ include 目录意外传导,
  现按 v2 依赖规则显式声明 proto;② dg_net 补 services/ 目录(跨子目录 include);
  ③ 组件 README/头注释自引用清理。
- **验收:WSL Ubuntu-22.04 全新构建 22/22 ctest 全过、0 警告**。
- 文档同步:PROJECT_PLAN §3.1/§3.4/快照、architecture.md §1/§2.1/§4、door-guard/README 索引。

**没做完 / 坑**
- Windows 本机 push 仍被拒(id_rsa.pub 未注册到 GitHub)——注册后 `git push` 即可。
- 教训:dg_log 的 PUBLIC include 目录曾是全体目标的 proto 可见性来源(暗依赖),
  迁移时必须排查"链接链继承的 include 目录",不能只看源码 include 语句。

**下一步**
- M2 行为升级(每项独立提交):registry 装配+看门狗+降级矩阵 / config 双文件+旧配置迁移 /
  DB WAL+单写者+特征缓存 / OTA 按需线程。

---

## 2026-09-20 架构 v2 评审稿(重构方向修订)

**做了什么**
- 评审了用户的重构方向初稿(五层栈:components/drv→modules→services→UI + 双注册表 +
  线程分配),结论:方向对,但有一处自相矛盾 + 三处缺失,产出修订稿
  **`docs/architecture-v2-proposal.md`**(评审稿,未动任何代码)。
- 关键修正:耗时任务不再进 tasker(收敛 ≤500ms,长任务一律独立线程,OTA 按需);
  补 proto 契约层与 tests 落位;liveness 显式化为认证管线强制阶段;
  register→registry(C 关键字)、vertify→verify、meun→menu、stream→capture;
  线程睡眠规范(condvar/eventfd + 原子状态,禁标志位忙等);线程↔服务归属表;
  main 转看门狗 + 降级矩阵;cur_config 移 /userdata/doorguard(A/B 不丢配置);
  配置全量进内存 + 防抖原子落盘(弃"配置分页");DB 补 WAL + 单写者队列 + 特征全量缓存;
  NTP 立项;services→drv(npu)白名单 + 只读跨服务直调双白名单。
- 文档内含:目标目录树、依赖白名单、迁移映射表(现→目标)、M0~M3 迁移阶段。

**没做完 / 待定**
- 仅方案,**零代码迁移**;4 项待用户拍板(只读直调放宽 / device_config 表冻结 /
  RTSP 暂缓 / 迁移时机),见 proposal §10。

**下一步**
- 用户评审 proposal → 并入 PROJECT_PLAN §三 + architecture.md → M1 机械迁移(零行为变更,
  全测试回归)→ M2 行为升级逐项独立提交。

---

## 2026-09-18 web 上位机迁移到 Vue 3(模块化重构)

**做了什么**
- 前端重写为 **Vue 3 + Vite** 工程(`modules/net/web/frontend/`),不再是一堆拼字符串的
  原生 JS。分层:`views → stores → api → components`,组件纯展示(props/emits)、
  HTTP 出口唯一(`api/client.js` 注入 token/处理 401)、状态用模块级单例 composable
  (不引 Pinia:共享状态就四处,少一层依赖体积)。
- 页面按视图拆分并加了路由:登录 / 设备概览(指标+实时事件+时间同步)/ 记录查询 /
  账号安全 / 固件升级 / 监控占位;`AppShell` 提供侧栏导航 + 顶栏(实时连接脉冲、
  默认口令横幅)。设计 token 沿用设备端蓝白主题(与 `ui/theme.h` 同源),动效保留并
  拆成 `styles/animations.css`。
- 固件侧改为**通用资源表**:`gen_pages.sh` 遍历 `pages/` 生成
  `DG_WEB_ASSETS[]`(路径→MIME+内容,非 ASCII 全转义为八进制,每 512B 断行),
  `web_server.c` 用一个兜底处理器按精确路径查表;未知路径回落到单页应用、
  `/api/` 前缀才回 JSON 404。以后加图片/字体不用改固件代码。
- 依赖与产物纪律:`package.json` 锁精确版本;`pages/` 与 `web_pages.c` 入库,
  **只改后端 C 代码的构建机仍然不需要 node**;`build_frontend.sh --install` 一条命令重建。

**踩的坑(都已修,且有测试守着)**
- **`isPass(null)` 判成"通过"**(`Number(null)===0`):门禁界面把拒绝显示成通过是事故级
  错误 → 改显式比较,单测锁死。
- **实时列表 key 用 `ts+user_id` 拼字符串**:陌生人事件无 user_id → key 变 `NaN`,
  Vue 复用错行(有告警)→ 改由 store 打单调 id。
- **ToastHost 引用了 store**:自家分层检查器当场报错 → 改成纯组件,由 `App.vue` 接线
  (检查器把"模块化"变成了可执行约束,不再靠自觉)。
- **会话中途失效没人管**:401 只清状态不跳转,用户会对着不再更新的页面干等 →
  路由层订阅 `onUnauthorized` 送回登录页并记住来源。
- **vitest 里 `vi.resetModules()` 后重复 import 会拿到新实例**(store 单例被绕过)、
  **应用侧按需 import 视图需要真实 I/O 轮次**(断言跳转要用 `vi.waitFor`)。
  两者都是测试环境特性,不是产品缺陷——为确认这点,补了 **构建产物冒烟测试**:
  把 `pages/assets/app.js` 直接丢进 jsdom 执行,跑通"挂载→登录→概览→WS 事件上屏"。
- **验收脚本抓到"二进制陈旧"**:服务端吐出的字节与仓库里的前端产物逐字节比对,
  第一次跑就失败(改了前端没重新构建固件)——这正是它要抓的。

**测试与验收**:前端 vitest **44 项**(api/stores/组件/集成/产物冒烟)、
静态检查 `frontend_check.py`(资源表一致、接口↔路由、分层、体积预算、离线)、
接口验收 `web_test.sh` **63 项**;PC 与交叉编译零告警,ctest 22 项全绿。
固件体积 +113KB(内嵌产物 146KB,JS 131KB)。

**未做**:浏览器像素级人工复核(本会话无浏览器后端;已用 jsdom 产物冒烟 + 静态检查
兜住"能不能跑/接线对不对",但"好不好看"仍需人眼过一次)、监控画面接 capture 帧、
前端多语言。

## 2026-09-18 web 上位机改造 + mDNS 做实(局域网按名字可用)

**做了什么**
- **mDNS 重写**(modules/net/mdns):拆出 `mdns_wire.c`(纯函数,可单测)+ 应答器状态机。
  现在公告 A + `_http._tcp` 的 PTR/SRV/TXT,能做**服务发现**(手机/avahi-browse 能看到设备);
  探测 3 次防重名(冲突自动改名 `doorguard-2` 并持久化)、通告 2 次、**IP 变化自动重通告**、
  关机发 goodbye;组播应答带 cache-flush,legacy(非 5353 端口)查询走**单播**应答(回带 ID/问题段、
  TTL 压到 10s);逐接口入组 + IP_PKTINFO 按来源网口应答。
- **web 上位机**:业务逻辑与鉴权重做——`web_auth`(凭据 + 登录风控)、`web_session`(token 表
  滑动续期/容量驱逐/改密即踢下线)、方法严格校验、日志查询加用户过滤与分页校验、
  NTP 改异步(202 + WS 结果)、`/api/account` 改账号口令(需旧口令)。
  页面拆成真前端文件 `pages/{index.html,app.css,app.js}` → `gen_pages.sh` → `web_pages.c`
  (蓝白主题 + 卡片入场/水波纹/toast/数字滚动/直播列表/LIVE 脉冲等动效,零外部依赖)。
- **设备菜单**:设备管理页新增 **Web 管理** 子页(服务状态、局域网地址 `http://doorguard.local:8080`、
  当前账号、默认口令告警、改账号/改口令带二次确认);UI 与 net 模块经事件通信
  (新增 EV_NET_WEB_STATE_REQ/STATE/SET/SET_RESULT),UI 不碰凭据存储。

**踩的坑(已修,勿回退)**
- **civetweb 在 OpenSSL 3 下 WebSocket 握手必崩**:`NO_SSL=1` 时它不包含 OpenSSL 头,
  却仍调 `EVP_Digest`/`EVP_get_digestbyname` → 隐式声明把返回指针截成 int → 段错误。
  修法:`third_party/civetweb/dg_openssl_shim.h` + CMake `-include`(civetweb 带 `-w`,告警全被压掉)。
- **WS 拒连丢状态行**:`mg_send_http_error` 先置 `conn->status_code`,后续 header 发送被跳过,
  客户端只收到裸 body → 改自己写完整 401。
- **`mg_set_request_handler("/")` 在模式匹配阶段匹配一切** `/api/**` 兜底永远不会被命中。
- **mDNS 线程自死锁**:持 `s_mtx` 时又调 `snapshot()`(非递归锁)→ 连带卡死 web 线程(现象:`/api/device` 永挂)。
- **NTP 服务从未被装配**(main 里只 include 了头):菜单/上位机的"时间矫正"一直静默无效 → 已补 holder 注册。
- 原 `/api/device` 的 `uptime_s` 是 `time(NULL)-0`(其实是 epoch);WS 端点原先**完全没鉴权**。
- 新增测试:test_web_auth(凭据/会话/风控 39 项)、test_mdns_wire(报文 69 项);
  `tests/web/web_test.sh` 扩到 **57 项**(自起服务、清沙箱库),新增 `ws_test.py`(服务端主动推送)、
  `mdns_query_test.py`(报文级)、`ui_static_test.py`(页面 id/路由/括号一致性,无浏览器也能查)。

**状态**:PC 端 22 项 ctest 全绿;web 验收 57/57;交叉编译零告警。**未做**:web 页面视觉人工复核
(本会话无浏览器后端,只做了静态与接口级验证)、监控画面(仍是占位,待接 capture 帧)。

---

## 2026-09-18 输入体系:字母键盘 + 每个输入都做合法性检测

- **背景**:设备无物理键盘,唯一输入是 5 寸触摸屏;原来只有数字键盘(dg_kbd),
  所以字母 ID/字母密码/英文姓名都输不了,而库里字段(J/SQL)本身不限字符集 ——
  "建得出、设备验不了"的隐患

- **键盘升级**(`ui/widgets/dg_kbd.c`):数字页(默认)+ **字母页**(QWERTY 三排 +
  ⇧ 大小写 + 空格 + ⌫ + OK),页脚 ABC↔123 切页。两页建好后用
  `LV_OBJ_FLAG_HIDDEN` 切换,**不删对象**——切页键就在这棵子树里,回调里删祖先
  会踩 LVGL「事件中途销毁对象」的坑(同类崩溃在待机覆盖层上实测过)。
  大小写只用 `dg_btn_set_label` 改显示文字,键值不变(新增该 API)
- **输入合法性检测(每个输入两条链路都过)**:
  - 规则唯一权威 `proto/valid.{h,c}`:user_id 3~31 位字母/数字/`-`/`_` 且首字符
    字母或数字;user_name 1~63 字节非空/无前后空格/无控制字符;password 4~31 位
    可见 ASCII 无空格。纯函数,宿主可直接单测
  - **UI 侧即时**:`dg_popup_input` 改配置式(标题/掩码/键盘初始页/`max_len`/
    `validate`),不合格 → 弹窗内**红字提示且不提交**(可就地改,不再等 5s 超时),
    长度上限直接设在 textarea 上(打不进超长值);文案包装在 `ui/valid_ui.c`
  - **存储层权威兜底**:`db_user_add/update/set_password` 同规则校验,
    新错误码 `ERR_BAD_UID/-NAME/-PWD(-26/-27/-28)`;设备/上位机/脚本/未来 API
    都绕不过
- **顺带修掉一个假功能**:设备管理的"网络配置/NTP"原来弹个输入框、输什么都回
  "NTP同步成功"。现 NTP 按钮改为**真触发一次**(新事件 `EV_NET_NTP_TRIGGER`,
  ntp_service 在独立线程里跑 chronyc,避免阻塞总线线程),结果经 `EV_NET_NTP_RESULT`
  回来由设备管理页显示(区分"设备未联网"/失败);不再让面板上的人现填服务器地址
- **测试**:`tests/test_valid.c`(规则逐条边界,含命令注入字符/32 位超长/中文密码)、
  `test_storage.c [S7]`(存储层拒绝非法 ID/姓名/密码)、`test_widgets` 扩到
  字母页/⇧ 大小写/**校验失败不提交**;dg-test 20/20,--tsan 20/20
- **文档**:spec-database §2.1(字段规则表,唯一权威)、spec-ui §6(键盘两页 + 校验 UX +
  弹窗表)、ui/README;新增中文文案后已重跑 `ui/font/gen.sh`

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
