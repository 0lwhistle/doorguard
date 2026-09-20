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
