# 拍摄录入 + 头像 — 交接文档(2026-09-21)

> **读本文前先读**:`docs/DEVLOG.md` 顶部三条 + `PROJECT_PLAN.md` §一 快照。
> 本文是"拍摄录入 + 头像"这一功能的**唯一交接事实源**:已完成/待做/坑/接口全在此。
> 依据快照:`master@f5e2771`(2026-09-21),29/29 ctest 绿,交叉编译零告警,已推板。

> ## ✅ 已完成(2026-09-21 当日两班做完,本文仅留档)
> §3 的 UI 三步已全部落地并合入 master:同帧拍照入库存图(照片槽)、拍摄页
> `page_capture`(质量实时提示+不合格禁拍+回看重拍)、`dg_avatar` 头像显示;
> `db_user_clear_face` 连带清头像;新增 `modules/jpeg`。测试 30/30、零告警、已推板。
> **剩余**:§6-1~7 需真人对镜头的板上人工验收;§3.4(事件泵帧率)与质量阈值
> 标定仍未做。坑与接口约定继续有效,下一班**不必按 §3 重做**。

## 0. 一句话状态

**数据与服务侧已完成并测过**(质量体系 + 头像加密入库 + 连续命中修复),
**UI 侧未做**:拍摄页、JPEG 编解码、列表/编辑页头像显示。用户要的效果是:
录入人脸时**看得到拍摄画面**、**点击拍摄**(而不是现在静默抓缓存),
拍下的照片作为头像在用户管理与用户编辑页显示预览。

## 1. 用户要的是什么(原话要点)

- 录入人脸时能在屏幕上看到拍摄视角,**点击拍下一张照片**的形式添加人脸;
- 拍下的图片在**用户管理**与**用户编辑**页能作为**头像预览**显示;
- 要有**质量体系**(防抖动模糊脸进库);
- 照片**存数据库**;加密与尺寸由开发者定(已定:**AES-256-CTR 加密,160×160**)。

## 2. 已完成(代码在工作树/已提交,测试覆盖)

### 2.1 连续命中循环修复(卡死/闪烁真凶)— `vision_rknn.c`
- **根因**:1:N 是持续上报的(每 300ms),FSM 无重复触发闸 → 开门→结果→回普通→
  再开门无限循环:继电器反复动作 + 日志刷屏 + 弹窗反复建销(UI 拖垮 = 卡死),
  脸框因状态反复切换而闪烁。**被 ROCKIVA 路线掩盖**(板上从没跑起来过)。
- **修法**:后端 `s_granted_presence` —— **一次在场只放行一次**,`FACE_LOST` 时重新武装
  (走开再回来 = 新的一次)。**不要在 FSM 里加冷却**:`FSM_EV_MATCH_1N` 传的 `now_ms`
  是 0(FSM 是时间注入式设计),冷却判定会算成负数而永久屏蔽(已踩过、已回退)。
- **待板上验收**:站在镜头前应只开一次门;走开再回来能再开。

### 2.2 人脸质量体系 — `services/vision/face_quality.c/h`(纯 C,宿主可测)
- 三因子:清晰度(灰度 Laplacian **方差**)、人脸框较小边像素、检测置信度。
- **已接入识别路径**(`vision_rknn.c` 的 `recognize()`):测的是"要喂给 ArcFace 的那张
  对齐脸";不合格丢弃特征 + 2s 节流日志(脸 px / 清晰度 / 检测分 / 三个阈值)。
  录入抓取用同一份缓存特征,所以**同时保护了 1:N 与录入两条路**。
- 阈值走配置(零魔数):`face.min_face_px`=80、`face.blur_min`=**50(须标定)**、
  `face.det_score_min`=0.70;阈值 **0 = 该项不启用**(便于先只开清晰度标定)。
- 测试 `tests/test_face_quality.c` 19 项,关键是验证**指标真能区分锐/糊**
  (合成棋盘图锐=61516 vs 两次均值模糊=35.7)。
- **待板上标定**:看日志 `质量闸门拦下(...)` 与 `清晰度 N` 的实测值调 `cur_config.json`。

### 2.3 头像加密入库 — `modules/sqlite/storage.c` + `proto/types.h`
- `users` 表加 `avatar BLOB`(**追加在最后**,新库与迁移库列序一致)+ **幂等迁移**
  (PRAGMA table_info 检查 → ALTER TABLE);板上已执行。
- **加密**:复用 `dg_feature_wrap/unwrap`(通用 AES-256-CTR BLOB 封装,随机 16B IV 前缀)。
  决策理由:人脸照片与特征同属生物特征数据,库文件泄露时不该只有特征受保护。
- **独立接口,不塞进 `user_rec_t`**(那是认证/检索热路径,每次整份拷贝):
  ```c
  int db_user_set_avatar(const char *user_id, const uint8_t *jpeg, size_t len);  /* len=0=清除 */
  int db_user_get_avatar(const char *user_id, uint8_t *out, size_t cap, size_t *out_len);
  /* 返回 DG_ERR_NOT_FOUND = 用户不存在 **或** 未录头像(调用方显示占位) */
  ```
  上限 `32KB`(超限 `DG_ERR_PARAM`,不撑大库)。删用户随行记录消失。
- 测试在 `tests/test_storage.c` 的 `[S8]`:往返、覆盖写(变长)、清除、超限、
  用户不存在/参数非法、**密文落库验证**。

### 2.4 尺寸与格式决策
- **160×160 JPEG(q≈80,约 6~10KB)**。依据:列表缩略图 ~60px、编辑页预览 ~200px
  都清晰;2000 人 ≈ 20MB。识别用的 112×112 太小(放大会糊),不要直接拿来当头像。
- 生成方式建议:用 `rknn_align_plan()` 按 112 参考算矩阵后**整体缩放 160/112**
  (`M' = S·M`,S=diag(s,s,1)),再 `rknn_align_warp(..., 160, 160)` —— 复用现成对齐,
  且全库头像构图一致。

## 3. 待做(UI 侧,按序)

### 3.1 采集侧:拍照与编码
- 视觉后端**已缓存"最近一次对齐 112×112 的人脸 RGB + 时间戳"**?**没有——这一步要加**。
  照现有 `s_cap`(特征缓存)的写法加一个同级 `s_snap`(mutex + RGB 缓冲 + 时间戳),
  在 `recognize()` 通过质量闸门后顺便 memcpy(112×112×3 = 37KB,约 0.1ms,无感)。
- **头像与特征必须出自同一帧**(同一时刻的 `s_snap` + `s_cap`),否则脸动了一下
  就对不上号。
- **编码**:`libjpeg` 板上已有(sysroot `jpeglib.h`、板 `/usr/lib/libjpeg.so.8`)。
  编码只在**用户点击拍摄时**发生一次(不是每帧),放在 `on_capture_req`
  (**事件总线线程**,不是相机线程)里做几毫秒编码无妨。
- 传参:现 `vision_service_submit_feature(uid, seq, feat, len)` 只带特征;
  需要让照片一起到达 enroll 服务(照片是 KB 级,按架构纪律**不进事件总线**,
  走槽位/环形缓冲)。两种做法二选一,推荐 **A**:
  - **A**(改动小):给 `vision_service` 加一个"最近照片槽"(单槽 + seq),
    后端在 `submit_feature` 前先 `vision_service_put_avatar(seq, jpeg, len)`,
    enroll 服务在 `EV_VISION_FEATURE` 处理时按 seq 取回并 `db_user_set_avatar()`;
  - B:扩展 `vision_service_submit_feature` 签名带上照片指针(会动到契约与测试)。

### 3.2 拍摄页(新页面)
- **照抄主页的预览方式**(`ui/pages/page_home.c`):
  `camera_latest()` → `lv_canvas_create` + `lv_canvas_set_buffer(LV_IMG_CF_TRUE_COLOR)`
  + `lv_canvas_copy_buf()`,在自己的定时器里刷。
- 布局:`[拍摄]`(人脸合格才可点)+ `[取消]` + 质量提示行(不合格时显示
  "太模糊,请保持不动"/"请靠近一些"/"请正对镜头",文案全走 `_()`)。
- 质量状态来源:后端已有 `s_last_q` / `s_last_q_verdict`(`face_quality.h` 的枚举),
  需要**经事件提供给 UI**——建议新增一个轻量事件(如 `EV_VISION_QUALITY`,
  携带 verdict + 脸框),**只在拍摄页打开时订阅**(避免平时刷总线)。
- 流程:编辑页"人脸·录入" → push 拍摄页 → 点拍摄 → 发 `EV_ENROLL_REQUEST` +
  等 `EV_ENROLL_RESULT` → 显示刚拍的照片 + `[重拍] [完成]`。
- **`NAV_MAX_PAGES` 当前 9,已注册 9 个页** → 加页面**必须同时改 `ui/navigator/navigator.h`**,
  否则 `navigator_register` 返回 `NAV_ERR_FULL`(静默注册不上的坑)。
- 编辑页现有的"录入 5s 超时"逻辑(`page_user_edit.c`)要和拍摄页流程对齐。

### 3.3 头像显示
- **LVGL 的 `LV_USE_PNG=0`、`LV_USE_SJPG=0`、`LV_USE_FS_POSIX/STDIO=0` 全是关的**
  (`third_party/lvgl/lv_conf.h`)。两条路:
  - **推荐**:用 `libjpeg` 解码到 RGB565 缓冲,再包装成 `lv_img_dsc_t`
    (`LV_IMG_CF_TRUE_COLOR`)显示——不解码器也能显示,`lv_img_set_src(&dsc)` 即可;
    libjpeg 支持 1/2、1/4、1/8 缩放解码,缩略图用它很省。
  - 或打开 `LV_USE_SJPG`(需 LVGL 的 SJPG 转换格式,要额外的转换步骤,不推荐)。
- 建议新增 `ui/widgets/dg_avatar.c`:统一"uid → `db_user_get_avatar` → 解码 →
  `lv_img_dsc_t` → 显示",带小缓存(列表 6 行 × 缩略图,别每帧重解码)。
- 列表行:`dg_list_add_row(list, icon, text, cb)` 的 `icon` 位现在是 NULL,
  可扩一个带图片的行接口(或直接用 `lv_img` 叠在行上)。
- 编辑页:在"人脸"行旁显示头像预览;无头像显示占位(`_("无")` 已有)。

### 3.4 顺手可做(与本次症状相关)
- **脸框"卡顿"的平滑度上限是 UI 事件泵**(`ui/ui_evt` 的泵定时器 100ms = 10Hz),
  提高泵频率(如 33ms)可明显改善;后端 10Hz 节流(`RKNN_PUB_MS`)也相应可提。
  注意别把 LVGL 线程压垮——先测帧时再定。

## 4. 坑(全项目通用,务必先读)

1. **i18n 是硬约束**:任何新 UI 文案必须进 `ui/lang/zh-CN.json` 与 `en-US.json`,
   然后**跑 `ui/font/gen.sh` 重生成中文字体**(node/npx 在 WSL 可用;
   脚本已修 `--symbols` 前导空格,否则首字符"—"会被 npx 当参数)。
   漏了这步屏幕上就是方块——`tests/test_i18n.c` 会拦住。
2. **`test_i18n` 的"裸中文"扫描连注释都查**:中文注释里**不要用 ASCII 引号**
   (`"..."`),要用中文引号(`“...”`),否则误判为未包裹字面量。
3. **LVGL `lv_event_get_user_data(e)` 返回的是回调注册时的参数**,不是
   `lv_obj_set_user_data` 设的值——要用 `lv_obj_get_user_data(lv_event_get_target(e))`。
   (踩过:导致编辑页全部显示"无"、保存必败。)
4. **ArcFace 未烤归一化**:必须喂 `(x-127.5)/127.5` 的 **F32**。直喂 uint8 会让所有
   embedding 高度相似且**无任何报错**(`rknn_rgb_norm_f32()` 已封装)。
5. **模型必须匹配目标平台**:RK3588 的 .rknn 在本板被驱动直接拒收
   (`This rknn model is for RK3588`)。重转步骤见 `door-guard/models/README.md` §⑤。
6. **`db_user_update` 的 `len=0 = 保留` 语义**:清空某字段要用专用接口
   (`db_user_clear_face`,头像用 `db_user_set_avatar(uid, NULL, 0)`)。
7. **WAL 模式下验证"数据落盘形态"要扫 `db.sqlite` 与 `db.sqlite-wal` 两个文件**,
   只扫主库会假通过(安全测试假通过比没有更糟)。

## 5. 环境与命令速查

```bash
cd /home/olwhistle/doorguard && source env/env.sh   # dg-* 进 PATH,自动探测工具链
dg-build                     # 交叉编译(零告警是硬要求)
dg-deploy                    # 推板 + 重启(默认 192.168.2.95)
ctest --test-dir door-guard/build-tests --output-on-failure   # 宿主 29 项
```

- 板端:`/userdata/doorguard/models/`(模型)、`/userdata/doorguard/cur_config.json`(现用配置)、
  `/var/lib/door-guard/door-guard.db`(DB,含 `device_config` 遗留键)、
  日志 `/var/log/door-guard.log`、服务 `/etc/init.d/S60doorguard {restart}`。
- 对拍工具(板端):`npu_probe`(模型规格/压测)、`rknn_det_test`、`rknn_rec_test`。
- 相关文档:`services/vision/README.md`(视觉三后端与链路)、`models/README.md`(模型与实测)、
  `drv/npu/README.md`(NPU 库接口)。

## 6. 验收标准(这个功能算做完的样子)

1. 用户管理 → 编辑页 → "人脸·录入" → **出现实时画面**;
2. 画面里有人脸时 `[拍摄]` 可点,太糊/太小/太远时**给出具体提示且不可点**;
3. 拍下后能看到刚拍的照片,可 `[重拍]`;
4. 完成后返回编辑页,该用户"人脸"显示"已录入",**头像预览可见**;
5. 用户管理列表该行**显示头像缩略图**;
6. 删用户 / 清除人脸后,头像随之消失,重新进入显示占位"无";
7. 主页刷脸:命中开门;**站着不动不会反复开门**;走开再回来可再开;
8. `ctest` 全绿、交叉编译零告警、新文案中英双语且**字体已重生成**(无方块)。
