# RK3576 官方 SDK 开发资源指南

> 适用:KickPi K7(RK3576)+ 官方 Linux 6.1 SDK。
> SDK 本体(19GB)只在编译 VM:`~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008`,
> 本指南所有路径均相对该目录。`docs/` 子目录里是已拷贝进本仓库的官方 PDF,可直接下载阅读。

## 0. 一页速查:我要做 X,用哪个?

| 你要做的事 | 用什么 | 位置 |
|---|---|---|
| 编固件/编内核/编 rootfs | `build.sh` | SDK 根目录 |
| 烧录固件 | `rkflash.sh` / Upgrade_Tool | SDK 根目录 / `tools/` |
| 摄像头取流 + 屏幕显示(应用层) | **RKADK** | `app/rkadk` |
| LVGL 界面 | lvgl_demo + LVGL 官方指南 | `app/lvgl_demo`,`docs/` 有 PDF |
| NPU 推理(C API) | rknpu2 runtime + examples | `external/rknpu2` |
| 模型转换(PC 端) | rknn-toolkit2 | `external/rknn-toolkit2` |
| 人脸识别(官方方案,**已采用**) | ROCKIVA | `external/iva` |
| ISP 3A / 画质调优 | camera_engine_rkaiq | `external/camera_engine_rkaiq` |
| 快速验证视频管线 | gstreamer-rockchip | `external/gstreamer-rockchip` |
| 图像缩放/裁剪/格式转换(硬件) | linux-rga | `external/linux-rga` |
| 硬件编解码 | mpp | `external/mpp` |
| 理解媒体管线原理 | rockit samples | `external/samples` |

---

## 1. 编译系统:`build.sh` / `rkflash.sh`

**是什么**:Rockchip 一键构建系统。首次运行会让你选板级 defconfig(选
`rockchip_rk3576_kickpi_k7_ubuntu_defconfig`,K7/K7C/K7S × buildroot/debian/ubuntu 三种 rootfs 都有),
之后按目标构建并打包成可烧录镜像。

**怎么用**(在 SDK 根目录):

```bash
./build.sh                  # 首次:交互选板型,然后执行默认目标 all(全量)
./build.sh help             # 查看全部可用目标
./build.sh kernel           # 只编内核(改 DTS 后用)
./build.sh uboot            # 只编 uboot
./build.sh rootfs           # 只编 rootfs
./build.sh firmware         # 打包分区镜像
./build.sh updateimg        # 打包整卡升级镜像 update.img
./build.sh cleanall         # 全部清理
```

- 板级配置实际位置:`device/rockchip/.chips/rk3576/rockchip_rk3576_kickpi_k7_*.defconfig`
  (内容:用哪个 DTS、哪种 rootfs,K7 = DTS `rk3576-kickpi-k7-linux`)
- 首次编译需要外网(rootfs 会装依赖);产物在 `output/` 下
- 详细流程看 `sdk-guide/docs/Rockchip_RK3576_Quick_Start_Linux_CN.pdf`(官方快速上手,权威)

**烧录**:`./rkflash.sh`(底层调 `tools/linux/Linux_Upgrade_Tool/upgrade_tool`),
板子进 Loader/Maskrom 模式后执行;Windows 端用 Rockchip 官方 RKDevTool(自行下载)。

**本项目对应**:Phase B4(编译)、B5(烧录)。

## 2. RKADK:应用开发套件(取流+显示最快路径)

**是什么**:Rockchip 把"摄像头取流(VI)→ 显示(VO)→ 编码(VENC)→ 播放器 → 存储"
封装成的简单 C 接口库,内部已是 V4L2+DRM 零拷贝管线。**门禁的预览+显示直接基于它,
不要自己从 V4L2 裸写。**

**位置**:`app/rkadk`,含 `include/`(API 头文件)、`examples/`(15 个可跑样例)、`docs/`(中英开发指南 PDF,已拷贝进本仓库)。

**关键 example**:

| example | 演示内容 | 对本项目 |
|---|---|---|
| `rkadk_stream_test.c` | 摄像头取流 | capture HAL 参考 |
| `rkadk_disp_test.c` | 视频上屏显示 | 预览链路参考 |
| `rkadk_ui_test.c` | UI 叠加(OSD) | 界面叠加参考 |
| `rkadk_media_test.c` | 取流+显示+编码组合 | 最接近门禁场景 |
| `rkadk_photo_test.c` | 拍照 | 人脸注册抓拍可参考 |

**怎么用**:阅读 `docs/Rockchip_Developer_Guide_Linux_RKADK_CN.pdf` 的 API 说明;
交叉编译用 SDK 的工具链(`prebuilts/` 下 aarch64 gcc),CMake 工程可直接引用其 CMakeLists 模式。

**本项目对应**:Phase B6(取流)、B8(预览上屏)的核心底座。

## 3. LVGL:图形界面

**是什么**:轻量级嵌入式 GUI 库。SDK 自带两份资源:

- `app/lvgl_demo`:厂家改好的 LVGL 工程(含 hal 抽象层、gallery/rk_demo 等示例),**作为我们 UI 应用的骨架起点**
- `buildroot/package/lvgl`:buildroot 集成包(产品化阶段用)

**怎么用**:先读 `sdk-guide/docs/Rockchip_Developer_Guide_Linux_LVGL_CN.pdf`(官方 LVGL 移植指南:
如何对接显示后端/触摸输入);显示后端对接 DRM(双平面方案见 PROJECT_PLAN.md §4.1),
触摸走 evdev(GT9xx 驱动内核原生)。

**本项目对应**:Phase B8(UI 上屏)。

## 4. 摄像头 / ISP:`camera_engine_rkaiq`

**是什么**:MIPI 摄像头的用户态 ISP 引擎,提供 3A(自动曝光/白平衡/降噪)。
IMX415 是 RAW 传感器,**没有它画面就是不正常的**。

**位置与关键内容**(`external/camera_engine_rkaiq`):

- `rkaiq/iqfiles/`:各传感器的 IQ 标定文件,**含 IMX415**(`imx415_*.xml` + 4K FEC/LDCH mesh)
- `rkaiq_3A_server`:板端 3A 服务进程(应用取流前要把它跑起来,或以库形式集成)
- `rkaiq_tool_server`:在线画质调优——板端跑它,PC 端用 IQ Tool 连上去实时调曝光/色彩/降噪,调完导出新 IQ xml

**怎么用**:先用 SDK 内的媒体样例把"rkaiq + 取流"跑通(参考 `external/samples` 的 vi 系列,
或 RKADK 内部已集成);量产前用 tool_server 调一版适配我们场景(室内逆光/夜间)的 IQ 文件。

**本项目对应**:Phase B6(出图)、B10(画质调优)。

## 5. 人脸识别:官方 ROCKIVA(本项目采用)+ NPU 工具链

**首选:ROCKIVA**(`external/iva`,Rockchip 官方智能视觉分析 SDK)——检测/关键点/人脸识别/
**1:N 检索(注册+搜索)**/属性分析(性别年龄表情等)一体,模型自带,NPU 推理:

- 板型适配:`librockiva/rockiva-rk3576-Linux`(预编译库)+ `models/rockiva_data_rk3576`
- API 头文件:`librockiva/rockiva-rk3576-Linux/include/rockiva_face_api.h`
- 开发指南:`Rockchip_Developer_Guide_ROCKIVA_SDK_CN.pdf`(已拷入本仓库 `sdk-guide/docs/`)
- Buildroot:`BR2_PACKAGE_IVA=y` + `BR2_PACKAGE_IVA_RK3576=y`(doorGuard 配置已启用,staging 可直接链接)

**NPU 工具链(备选/自定义模型时用)**:

| 组件 | 位置 | 干什么 | 跑在哪 |
|---|---|---|---|
| rknn-toolkit2 | `external/rknn-toolkit2` | 把 ONNX/TF 模型转成 `.rknn`(含量化) | PC(Linux,x86) |
| rknpu2 | `external/rknpu2` | 板端运行时 `librknnrt.so` + C API | 板子 |

examples(`external/rknpu2/examples/`):`rknn_api_demo`(基础流程)、`rknn_yolov5_demo`(检测模板)、
`rknn_benchmark`(耗时分析)。自定义模型从 GitHub 的 rknn_model_zoo 取(SCRFD/ArcFace 有现成转换脚本)。
量化校准用人脸图 100~500 张;量化后重标定比对阈值。

**本项目对应**:Phase B7(NPU 单项)、B9(人脸链路)。

## 6. 媒体中间件

| 组件 | 位置 | 是什么 | 何时用 |
|---|---|---|---|
| rockit samples | `external/samples` | RK 媒体管线的 C 样例集:vi(取流)/vo(显示)/venc(编码),`sample_demo_vi_venc.c` 等 | 理解管线原理;RKADK 不够用时下沉到这层 |
| gstreamer-rockchip | `external/gstreamer-rockchip` | 硬件加速 GStreamer 插件 | **快速验证**:`gst-launch-1.0` 一条命令打通取流→显示 |
| linux-rga | `external/linux-rga` | 2D 加速库(缩放/裁剪/格式转换) | NPU 前处理、预览缩放,全走它(CPU 不碰像素) |
| mpp | `external/mpp` | 硬件编解码(H.264/H.265) | 录像/网络推流才用,首版门禁用不到 |
| libmali | `external/libmali` | Mali GPU 用户态库 | LVGL 若切 GPU 渲染时用 |

## 7. 工具与厂家层

- `tools/linux/`:upgrade_tool(烧录)、Firmware_Merger/boot_merger(固件合并)、签名工具(SecureBoot/AVB,量产加密才用)
- `external/kickpi/`、`debian/overlay-kickpi/`:KickPi 厂家板级附加(硬件优化服务等)
- 板子规格书/原理图:在 Windows 共享文件夹 `share/k7/KICKPI-K7(规格书+原理图+机械图)/`
  ——**接指纹/IC 卡模块查引脚时必看**

## 8. 官方文档地图

**已拷贝进本仓库 `sdk-guide/docs/`(直接下载看)**:

| 文件 | 内容 | 优先级 |
|---|---|---|
| `Rockchip_RK3576_Quick_Start_Linux_CN.pdf` | 编译/烧录快速上手(权威流程) | ★★★ B4 前必读 |
| `Rockchip_RK3576_Linux6.1_SDK_Release_V1.0.0_20240620_CN.pdf` | SDK 发布说明(目录结构、依赖) | ★★ |
| `Rockchip_Developer_Guide_Linux_RKADK_CN.pdf`(+EN) | RKADK API 与示例 | ★★★ B6/B8 必读 |
| `Rockchip_Developer_Guide_Linux_LVGL_CN.pdf` | LVGL 移植指南(显示/输入对接) | ★★★ B8 必读 |
| `Rockchip_User_Guide_Linux_Rockit_CN.pdf` | rockit 媒体管线 | ★★ 需要下沉时读 |
| `Rockchip_User_Guide_Linux_Gstreamer_CN.pdf` | GStreamer 硬件插件 | ★ 快速验证用 |
| `Rockchip_Developer_Guide_Linux_Graphics_CN.pdf` | 图形栈架构(DRM/KMS) | ★★ 理解双平面 |
| `Rockchip_Developer_Guide_ROCKIVA_SDK_CN.pdf` | 官方人脸方案 API/集成指南 | ★★★ B7/B9 必读 |

**留在 SDK 里按需取**(`docs/cn/`,共 674MB 未搬运):Audio/Security/Recovery/System 等主题,
以及 `docs/en/` 英文版。NPU 文档在 `external/rknpu2/doc/`,toolkit2 文档在其仓库内。

## 9. 和项目计划的对应关系

见仓库根目录 `PROJECT_PLAN.md` 第五节 B1~B10。本指南 §1→B4/B5,§2→B6/B8,
§4→B6,§5→B7/B9,§3→B8。开工顺序不要乱:底座(B1~B5)→ 单项(B6~B7)→ 整合(B8~B9)→ 扩展(B10)。
