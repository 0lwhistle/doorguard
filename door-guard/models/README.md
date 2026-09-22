# models/ — 视觉模型清单与部署说明

> **本目录不放二进制。** 模型文件是 SDK 的几十 MB 级 data,不入 git(大文件纪律);
> 这里维护"来源、校验和、部署目标、特征口径"的清单,真文件从 SDK 拷。

## 来源(VM)

```
~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008/
    external/iva/librockiva/rockiva-rk3576-Linux/models/rockiva_data_rk3576/*.data
```

## 部署(板上,整目录)

- 运行时路径优先级(`services/vision/vision_rockiva.c`):
  `DG_IVA_MODEL_DIR`(env,S60 已默认 /usr/lib)> `device_config` 的 `face.model_dir` > `/usr/lib`
- ROCKIVA 的 modelPath 是**目录**:FACE_Init 在其下找一整套模型
  (`face_landmark5.data`、`face_quality_v2.data`、前级 `object_detection_v3_cls8.data`,
  后续还有识别模型)——**缺一个即整体失败**(FACE_Init 返回 -1,日志 ERROR,系统降级不崩)。
  因此必须**整目录拷入**,至少 face_landmark5 + face_quality_v2,建议全拷:

  ```bash
  scp <SDK>/rockiva_data_rk3576/*.data root@<板IP>:/usr/lib/
  ```

- 换模型 = 换特征空间,**必须同步改 `device_config` 的 `face.model_tag`**
  (当前 `rockiva-face-v1`),代码靠它拦截旧特征静默错配。

## 清单维护(拷贝后在 SDK 目录生成,回填本目录)

```bash
cd <SDK>/rockiva_data_rk3576 && sha256sum *.data > sha256sums.txt
```

把 `sha256sums.txt` 连同 SDK 版本(commit/tag)记录到本目录,后续换模型可校验一致性。

## 打包(B10)

固件阶段模型应随 rockiva 库同源进 buildroot 的 IVA 包(落 /usr/lib),不再手工拷;
模型格式与 rockiva 库版本绑定,二者必须同发。A/B 升级若另设分区/userdata 承载模型,
依赖 `face.model_tag` + `vision_backend=ERROR` 自诊断兜底,不会出现新旧静默错配。

## 已就位:自组 rknn 管线模型(2026-09-20,用户 rknn-toolkit2 转换)

| 文件 | 角色 | 出处 | 大小 |
|---|---|---|---|
| `RetinaFace.rknn` | 人脸检测(备选检测器) | RetinaFace | 18 MB |
| `det_10g.rknn` | 人脸检测(SCRFD-10G) | InsightFace model zoo | 9.4 MB |
| `w600k_r50.rknn` | 人脸特征提取(ArcFace-R50,w600k) | InsightFace model zoo | 84 MB |

校验:`sha256sums.txt`(二进制在本目录,已被 .gitignore 挡住不入库)。
用途:vision_backend 可插拔契约的**自组后端**(SCRFD/ArcFace)
——**已落位并成为板上主线**(`services/vision/vision_rknn.c`,2026-09-21 上板;
ROCKIVA 为备选)。实现约定(已按此落地):

- ArcFace-R50 输出 512 维 float32 = **2048 B**,`DG_FEATURE_MAX` 已提至 **2048**
  (proto/types.h;users.features 是 BLOB,免迁移);
- 特征空间 ≠ `rockiva-face-v1`,自组后端用**独立 `face.model_tag`**
  (当前:`rknn-arcface-r50-v1`)。

### 板上实测(2026-09-21,`tools/npu_probe.c`;板端 `/userdata/doorguard/models/`)

`npu_probe <model.rknn>` 打印真实张量规格并试跑/压测(`DG_NPU_BENCH=N`)。结论:

**① `RetinaFace.rknn` 不可用——它是 RK3588 模型。**

```
E RKNN: This rknn model is for RK3588, but current platform is RK3576
E RKNN: Import rknn model failed!   → npu_model_load 返回 NULL
```

转换时目标平台选成了 RK3588。**已于 2026-09-21 用 rknn-toolkit2 重转,新文件见 ⑤**——
重转后 320×320 输入、int8 量化,比 SCRFD 快 30 倍(见 ⑥),**RetinaFace 已成首选检测器**。
(`det_10g.rknn` 仍可用:同为 InsightFace 系,5 点输出契约一致,但当前是 F16 未量化、
640×640,慢得多;若要留作 A/B,按 ⑤ 同法重转即可。)`

**② `det_10g.rknn`(SCRFD-10G)可用**,输入 **640×640×3 NHWC F16**,9 个输出 =
3 个 stride × 3 个头(score/bbox/kps),**每位置 2 anchor**:

| stride | 网格 | anchor 数 | score | bbox | kps(5 点) |
|---|---|---|---|---|---|
| 8  | 80×80 | 12800 | `[12800,1]` | `[12800,4]` | `[12800,10]` |
| 16 | 40×40 | 3200  | `[3200,1]`  | `[3200,4]`  | `[3200,10]` |
| 32 | 20×20 | 800   | `[800,1]`   | `[800,4]`   | `[800,10]` |

输出顺序 = `[score_8, score_16, score_32, bbox_8, bbox_16, bbox_32, kps_8, kps_16, kps_32]`
(rknn_model_zoo 的 SCRFD 约定)。bbox/kps 是**相对 anchor 中心的距离×stride**,需按
stride 还原;零输入时 score≈0.02(无脸),符合预期。

**③ `w600k_r50.rknn` 可用**,输入 **112×112×3 NHWC F16**(与 InsightFace 对齐约定
一致),输出 `[1,512]` = 512 维 embedding,证实 ① 的 `DG_FEATURE_MAX` 必须提到 2048 B。

**④ 稳态耗时(压测 30~40 次)**:

| 模型 | 最快 | 均值 | 上限 |
|---|---|---|---|
| `RetinaFace_rk3576_i8` 320×320 | **5.8 ms** | **6.7 ms** | ~174 fps |
| det_10g 640×640(F16 未量化) | 177.6 ms | 187.7 ms | ~6 fps |
| w600k_r50 112×112 | 55.6 ms | 59.4 ms | ~18 fps |

用重转后的 RetinaFace:一次完整「检测+识别」≈ **66 ms(≈15 fps)**——**框跟踪顺滑**,
且检测只用 6.7ms,给活体/质量闸门留足预算。对比 SCRFD 路线的 240ms(≈4 fps),
差距来自三处:输入 320 vs 640(像素少 4 倍)、int8 量化 vs F16、模型体积 1.4MB vs 9.4MB。

SDK/驱动版本(板上实测):`api=2.0.0b0 (35a6907d79@2024-03-24) drv=0.9.8`。

### ⑤ RetinaFace 重转记录(2026-09-21,主机 `rknn-toolkit2 2.3.2`)

```bash
# 素材:rknn_model_zoo/examples/RetinaFace/model/RetinaFace_mobile320.onnx(320×320)
# 标定集:zoo 的 COCO 20 张子集 + 该例程 test.jpg = 21 张(单张标定估不准量化范围)
find <zoo>/datasets/COCO/subset -name '*.jpg' > calib.txt
python3 convert.py <zoo>/examples/RetinaFace/model/RetinaFace_mobile320.onnx \
        rk3576 i8 out/RetinaFace_rk3576_i8.rknn
```

- 例程 `convert.py` 已配 `mean_values=[[104,117,123]] std=[[1,1,1]]` → **归一化烤进图**,
  所以运行时喂**原始 uint8 RGB** 即可(`npu_model_run(..., DG_NPU_TYPE_U8)`),
  不必自己在 C 里做 F16 归一化(SCRFD 那种没烤归一化的才要)。
- 预处理照 zoo 参考:`letterbox` 到 320×320、**补边值 114**、BGR→RGB。
- 转换警告:输入 dtype float32→int8;`onnx::Conv_613` 有 outlier(21.96),
  可能影响量化精度——**上线前用真实人脸比对一下检出率**。
- 板上实测:输入 `320×320×3 I8 NHWC`(quant scale=1.074510 zp=-14);
  3 个输出 = `[1,4200,4]` 框 / `[1,4200,2]` 分数 / `[1,4200,10]` 5 关键点。
- 解码已实现并对齐 zoo 参考:`services/vision/rknn_face.c` 的
  `rknn_retinaface_decode()`(PriorBox:min_sizes=[[16,32],[64,128],[256,512]]、
  steps=[8,16,32]、中心 +0.5 偏移、variance=[0.1,0.2]);宿主单测见
  `tests/test_rknn_face.c`(锚框数 320→4200 与板上实测互证)。

### ⑥ 板上教训:检测输入必须正立(2026-09-22)

模型对小模型(i8 mobilenet)的**方向极其敏感**:喂 ±90° 横置脸(摄像头横装
时的原始帧)时,离线对拍的正立图 0.999 会掉到 0.5~0.7,关键点/框回归出现
「摆正幻觉」(横脸的眼线被回归成水平、框近方形巨大),空场景还冒 0.5x 幻检。
链路上已用 `npu_pre_nv12_rotate` 先旋到预览同向再送检(见 vision README),
**任何后续改动都不得把原始帧直接送这个模型**;转换时的 Conv_613 outlier 警告
(见 ⑤)疑似也与该劣化有关,重转模型时应用横置/正立两组真实人脸分别对拍。
