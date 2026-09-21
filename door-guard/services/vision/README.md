# vision — 视觉服务与可插拔后端

## 职责

- **vision_service**(本模块的服务层):工作模式状态、特征槽(8 个环形槽)、
  库维护转发、录入抓取请求。**它不认识任何具体模型/推理框架**。
- **后端**(可插拔):真正出特征的东西。当前有 **三个实现**(注册顺序 = 优先级,
  缺省用第一个;运行时可用 `cfg face.backend` / env `DG_VISION_BACKEND` 按名字切):

| 后端 | 文件 | 定位 | 状态 |
|---|---|---|---|
| `rknn` | `vision_rknn.c` | **自组 rknn 主线**:RetinaFace 检测 + ArcFace 识别 | 板上跑通(2026-09-21) |
| `rockiva` | `vision_rockiva.c` | Rockchip 官方 ROCKIVA(检测/关键点/识别/1:N 一体) | 备选;本版 SDK 缺人脸模型 → 自行降级 ERROR |
| `sim` | `vision_sim.c` | PC mock(伪特征) | 只用于 UI/FSM 链路验证(PC 模拟器) |

  三者都只实现契约、都不认识业务:换后端不改服务层/FSM/UI/存储一行。

### rknn 后端的链路与分层(改它之前先读这段)

```
camera NV12 1280×720
  ├─[每帧] RGA letterbox 320×320 → RetinaFace@NPU(6.7ms)→ rknn_retinaface_decode
  │        → NMS → 最大脸 → 逆映射+旋到竖屏 → EV_VISION_FACE_BOX(10Hz)
  └─[每 300ms 且非 IDLE] NV12 原分辨率裁 ROI → 5 点对齐 112×112
           → (x-127.5)/127.5 → ArcFace@NPU(56ms)→ 512 维 → L2
           → 录入缓存 / 1:1 比对 / 1:N 检索(内存特征库暴力余弦)
```

分层与"在哪测":

| 层 | 文件 | 内容 | 怎么验 |
|---|---|---|---|
| 驱动 | `drv/npu/npu_model.c` | rknn 运行时薄封装(全仓唯一 include `<rknn_api.h>`) | `tools/npu_probe` |
| 驱动 | `drv/npu/npu_pre.c` | RGA letterbox + ROI 裁剪(坐标数学头内联) | `tests/test_npu_pre`(数学) |
| 算法 | `rknn_face.c` | SCRFD/RetinaFace 解码、NMS、5 点对齐、余弦、归一化 | `tests/test_rknn_face` |
| 装配 | `vision_rknn.c` | 契约实现、调度、事件发布 | 板上(npu_probe/rknn_det_test/rknn_rec_test) |

**两条血的教训**(踩过、有实测数据,别再踩):
1. **ArcFace 未烤归一化**:必须喂 `(x-127.5)/127.5` 的 F32。直喂 uint8 会让所有
   embedding 高度相似(cos(脸,纯色)≈0.79),识别永不命中且**没有任何报错**。
2. **模型目标平台**:RetinaFace 原始件是 RK3588 的,本板驱动直接拒收
   (`This rknn model is for RK3588`);重转见 `door-guard/models/README.md` §⑤。

```
access_service ──EV_VISION_SET_MODE──▶ vision_service ──ops->on_mode──▶ 后端
enroll_service ──vision_service_library_add/remove──▶ ops->lib_add/lib_del
                                              │
后端 ──EV_VISION_FACE_BOX / MATCH_1N / VERIFY_11──▶ 总线 ──▶ access/UI
```

## 契约(唯一接口:`vision_backend.h`)

后端实现 `vision_backend_ops_t` 并在装配层(`app/main.c`)注册:

| 字段 | 义务 |
|---|---|
| `name` | 后端名;`cfg face.backend` / env `DG_VISION_BACKEND` 按它选择 |
| `model_tag` | 特征口径(模型指纹);换模型必须换 tag,见下"换模型" |
| `has_landmarks` | 是否回灌关键点(B8 活体依赖);false 且 `liveness_enable=1` 会告警 |
| `start(enable_mock)` | 装配私有接线 + 初始化;失败返回非 0 → `vision_backend=ERROR` 降级 |
| `lib_add` / `lib_del` | 特征库增删(可 NULL);幂等,删不存在的 id 视为成功 |
| `compare` | 查重比较器(可 NULL=逐字节);1=重复 0=不重复 <0=错误 |
| `on_mode` | 工作模式变更(可 NULL):VERIFY_11 时按 uid 准备比对标的 |

硬性义务(违反会在换模型/降级时爆):

1. 单条特征 ≤ `DG_FEATURE_MAX`(512);超限**必须显式报错并丢弃**,不得静默;
2. 出站一律 `EVENT_BUS_PUBLISH`(回调在推理线程,禁止碰 LVGL/长事务);
3. 发布 1:N/1:1 命中前必须查 `vision_service_features_compatible()`,
   false 时不得发布 —— **宁可不开门,不可错开门**;
4. 帧回调里未就绪/推不动的帧必须立刻 `camera_nv12_release()`,否则 4 个 V4L2
   缓冲耗尽,相机永久断流。

服务层负责的一次性注入(后端不必自己做):`storage_set_feature_cmp(compare)`、
lib ops 转发、模式钩子、口径校验。

回归:`tests/test_vision_backend.c`(假后端驱动:注册/转发/比较器注入/口径拦截)、
`tests/test_vision_mode.c`(FSM 状态 → 模式联动)。

## 换模型(两种情形)

### A. 同框架换模型文件(最常见的"换一套 .data")

ROCKIVA 的 `modelPath` 是**目录**,换模型 = 换该目录下的文件集:

1. `cfg face.model_dir`(或 env `DG_IVA_MODEL_DIR`)指向新目录,默认 `/usr/lib`;
2. **`cfg face.model_tag` 改成新标识**(如 `rockiva-face-v2`),或者在
   `device_config` 表把 `face_model_tag` 改成新值 —— 因为换了模型 = 换了特征空间,
   库里旧特征与新模型比对出来的分数没有意义;
3. 重启后若检测到口径不一致,日志会打 ERROR 并**屏蔽命中**,直到重新录入全部人脸
   (然后按提示把 `face_model_tag` 更新为新值)。这是有意的:错开门比不开门严重。

同框架内换模型时不必改代码,只需换文件 + 换 tag。

### B. 换框架(用开源模型)

RK3576 上的通行做法(见 `sdk-guide/README.md §5`):

| 环节 | 开源/官方方案 | 产物 |
|---|---|---|
| 检测 | SCRFD / RetinaFace(rknn_model_zoo 有转换脚本) | `det.rknn` |
| 关键点 | PFLD / InsightFace 关键点 | `landmark.rknn`(B8 活体要) |
| 识别 | ArcFace / MobileFaceNet | `rec.rknn`(512 维浮点 embedding) |
| 运行时 | `rknpu2` 的 `librknnrt.so` + `rknn_api.h` | 板端推理 |

自建后端要注意的差异点:

- **特征长度**:ArcFace 类 512 维 float32 = 2048 B > `DG_FEATURE_MAX=512`,
  要把 `proto/types.h` 的 `DG_FEATURE_MAX` 提到实际长度(表是 BLOB,免迁移);
  服务层的槽位/库/DB 都是按这个宏走长度校验的,提一处即可;
- **比较器**:embedding 用余弦相似度阈值得分(0~1),自己写 `compare` 注入,
  阈值建议单独一个 cfg 键,不要复用 ROCKIVA 的 0.42;
- **关键点**:`has_landmarks=true` 时按帧 `liveness_service_on_face()`
  (万分比坐标,与分辨率无关);没有关键点就置 false,B8 活体在该后端上不可用;
- **模型文件路径**:自己的 cfg 键或 env,别复用 `DG_IVA_MODEL_DIR`;
- **资格**:`model_tag` 定成 `"rknn-arcface-v1"` 这类稳定标识。

### 新增一个后端的步骤(不改服务层/UI/FSM/存储)

1. 写 `modules/vision/vision_rknn.c`:`#include "vision_backend.h"`,实现 ops,
   文件尾导出 `const vision_backend_ops_t vision_backend_rknn = {...}`;
2. `CMakeLists.txt`:`DG_SIM OR DG_BUILD_TESTS` 之外的分支里加该源文件与它的
   头/库(rknn 头在 sysroot `usr/include/rknn`,库 `librknnrt.so`);
3. `app/main.c` 的 `mod_vision_backend()` 里注册(那时它已知道具体后端);
4. 跑 `tests/test_vision_backend.c` 的同类断言(可复制假后端写法)。

服务层、access/FSM、enroll、UI、storage 一行都不用动 —— 这就是这个契约的意义。

## 配置键(见 `config/README.md`)

| 键 | 含义 | 默认 |
|---|---|---|
| `face.backend` | 想要的后端名,空 = 第一个注册的(rknn) | 空 |
| `face.model_dir` | ROCKIVA 的模型目录(env `DG_IVA_MODEL_DIR` 优先) | `/usr/lib` |
| `face.model_tag` | 特征口径标识,空 = 用后端自带默认 | 空 |
| `face.match_threshold` | 1:N/1:1 命中阈值(余弦分度,须实测标定) | 0.42 |
| `face.face_dup_threshold` | 录入查重阈值(同上) | 0.90 |
| `face.liveness_enable` | 动作活体开关(B8 算法) | 0 |

rknn 后端的模型路径走 **env**(沿用 ROCKIVA 那套"现场诊断"惯例;模型文件与解码器
强绑定,文件名是后端身份的一部分):

| env | 含义 | 默认 |
|---|---|---|
| `DG_RKNN_MODEL_DIR` | 模型目录 | `/userdata/doorguard/models` |
| `DG_RKNN_FACE_MODEL` | 检测模型文件名 | `RetinaFace_rk3576_i8.rknn` |
| `DG_RKNN_REC_MODEL` | 识别模型文件名 | `w600k_r50.rknn` |

## 已知缺口 / 下一步

- **质量闸门未上**(检测分 + 最小脸尺寸 + 清晰度):防抖动模糊脸进识别造成误判,
  同时保护 1:N 检索与录入抓取两条路;位置见 architecture-v2-proposal 数据流图;
- **阈值标定**:`face.match_threshold`/`face_dup_threshold` 的余弦分度需按板上
  2s 节流日志"1:N 最高分"实测(ROCKIVA 时代的 0.42/0.90 只是起点);
- B7:活体判定算法(B8);`face.model_tag` 的切换目前靠配置/DB 手工改,
  将来可在设备设置页加一项(需要时再说);
- B10:把视觉模型放进 buildroot 包,别再手工拷(ROCKIVA 走 IVA 包;
  rknn 自组模型考虑随固件入 `/userdata` 或整包下发)。
