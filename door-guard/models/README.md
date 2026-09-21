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
用途:vision_backend 可插拔契约的**自组后端**(SCRFD/ArcFace,PROJECT_PLAN §4.3 备选路径)
——**代码侧尚无该后端**,B7 主线仍是 ROCKIVA;此三件为预置,落位待后端实现。实现时必做:

- ArcFace-R50 输出 512 维 float32 = **2048 B**,超过 `DG_FEATURE_MAX(512)`
  (proto/types.h),须先提上限(users.features 是 BLOB,免迁移);
- 特征空间 ≠ `rockiva-face-v1`,自组后端须用**独立 `face.model_tag`**(如 `arcface-r50-v1`)。

### 板上实测(2026-09-21,`tools/npu_probe.c`;板端 `/userdata/doorguard/models/`)

`npu_probe <model.rknn>` 打印真实张量规格并试跑/压测(`DG_NPU_BENCH=N`)。结论:

**① `RetinaFace.rknn` 不可用——它是 RK3588 模型。**

```
E RKNN: This rknn model is for RK3588, but current platform is RK3576
E RKNN: Import rknn model failed!   → npu_model_load 返回 NULL
```

转换时目标平台选成了 RK3588。要用它必须用 rknn-toolkit2 **重新按 RK3576 转换**。
当前检测器用 `det_10g.rknn`(同为 InsightFace 系,输出契约一致,见下)。

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

**④ 稳态耗时(压测 30 次)**:

| 模型 | 最快 | 均值 | 上限 |
|---|---|---|---|
| det_10g 640×640 | 177.6 ms | 187.7 ms | ~6 fps |
| w600k_r50 112×112 | 55.6 ms | 59.4 ms | ~18 fps |

一次完整「检测+识别」≈ 240 ms(≈4 fps)。门禁场景(站定刷脸)可用,但**盒子跟踪不顺滑**。
提速杠杆(按收益排序,均需 VM 侧重转):

1. **int8 量化**(带标定集重转):输入输出都是 F16 提示当前很可能是 FP16 图,
   6 TOPS 是 int8 口径,量化后通常有数倍收益——**收益最大的一步**;
2. **降输入分辨率**:门口人脸离镜头近,320×320 足够,算量约降 4 倍;
3. 软件侧缓解(无需重转):每 N 帧检测一次 + 帧间沿用上次框。

SDK/驱动版本(板上实测):`api=2.0.0b0 (35a6907d79@2024-03-24) drv=0.9.8`。
