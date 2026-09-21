# npu — NPU 推理库(RKNN 运行时薄封装)

架构里 `drv/npu` 的预留位终于填上了:2026-09-21 自组 rknn 人脸后端(会话①)落位。

## 定位(为什么是"薄封装")

只回答**"怎么把一个 .rknn 跑起来"**:加载 / 查张量 / 喂输入 / 推理 / 取输出 / 释放。

**不认识人脸**——不认识 RetinaFace/SCRFD/ArcFace,任何 rknn 模型(检测、识别、
关键点、将来的活体)共用本库。模型专属的后处理(解码/NMS/对齐/比对)留在
`services/vision/`,不放这里:驱动层一旦认识具体模型,换个模型就得改驱动,
那是分层塌陷(与 `drv/gpio`、`drv/uart` 同一纪律)。

**全仓只有 `npu_model.c` 一个文件 include `<rknn_api.h>`**。上层只认
`npu_model.h` 的类型与函数;换推理运行时(别的 NPU 栈)只改这一个 .c,接口不变。

## 接口

```c
#include "npu_model.h"

npu_model_t *m = npu_model_load("/userdata/doorguard/models/det_10g.rknn");
if (!m) return DG_ERR_IO;               /* 原因已进日志 */

npu_attr_t in;
npu_model_input_attr(m, 0, &in);        /* 输入尺寸/布局/类型随模型文件走,不写死 */

/* 按 in.width/in.height/in.channels 准备缓冲(原始 uint8 图 / 已归一化的 F16,
 * 两者字节数可能相同 → 必须显式声明缓冲里是什么,喂错会静默出错图) */
npu_model_run(m, buf, in.nbytes, DG_NPU_TYPE_U8);

npu_attr_t out;
npu_model_output_attr(m, 0, &out);      /* out.elems / out.dims 解码用 */
float *f = malloc(out.elems * sizeof(float));
npu_model_output_f32(m, 0, f, out.elems, NULL);   /* 驱动已反量化 */

npu_model_release(m);
```

错误码一律 `proto/err.h` 的 `DG_ERR_*`——rknn 的负值不泄漏给上层(调用方不该
认识 `RKNN_ERR_*`)。尺寸不符/容量不足**显式报错,不静默截断**。

## 设计要点

- **输入尺寸不写死**:转换时的输入分辨率/布局/类型随 .rknn 文件走,加载后
  `npu_model_input_attr()` 查出来。换模型(含换分辨率)不必改代码,也不必知道
  当初 rknn-toolkit2 是怎么配的。
- **反量化交给驱动**:输出用 `want_float=1` 取,上层只处理 float32,不必自己
  实现 `(q - zp) * scale`。`npu_attr_t` 仍带 zp/scale 供需要原样看的场合。
- **句柄非线程安全**:rknn context 不是线程安全的,一个 `npu_model_t` 只在一个
  线程里用;要并发就各自 load。这是硬约束,不要靠加锁绕过。
- **输出生命周期**:持有"最近一次"输出,下一次 `run` 或 `release` 时归还
  —— 每帧取完就拷走,别留着跨帧用(每帧泄漏一块是这类封装的经典坑)。

## 板上探针(换模型/联调第一件事)

`tools/npu_probe.c` 打得进板子直接跑,把 .rknn 的真实规格打出来:

```bash
# WSL 交叉编译后推板(dg-deploy 只管 door-guard 主程序,探针单独 scp)
scp door-guard/build/npu_probe root@<IP>:/root/
ssh root@<IP> "/root/npu_probe /userdata/doorguard/models/*.rknn"
```

输出:每个模型的输入/输出张量尺寸、布局、类型、量化参数、SDK 版本。
**换模型时先跑它**——输入尺寸是后处理解码的前提,别靠猜。

## 坑

- **宿主编不了**:宿主没有 rknn 头/驱动,CMake 里 `DG_SIM OR DG_BUILD_TESTS`
  不编本库(与 `vision_rockiva.c` 同一条守卫)。宿主上测的是 `services/vision/`
  里纯 C 的解码/对齐单元,不是这里。
- **驱动版本要与模型匹配**:RKNN 模型带 SDK 版本,与板上 `librknnrt.so` 差太远
  会 `RKNN_ERR_DEVICE_UNMATCH`(-10);`npu_hal_version()` 打印两边版本,排障先看它。
- **模型文件位置**:板上放持久分区 `/userdata/doorguard/models/`(A/B 升级会整
  分区替换 `/usr/lib`,放那儿升级即丢)。
