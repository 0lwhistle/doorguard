# RK3576 K7 人脸识别门禁系统 — 项目方案与执行手册

> 更新:2026-09-17(B4 固件产出)。本文档是项目的唯一事实来源(Single Source of Truth)。
> 每次会话开工前先读本文档恢复上下文;完成阶段后更新"进度快照"。

---

## 一、项目快照(每次开工先看这里)

| 项 | 状态 |
|---|---|
| 开发板 | KickPi **K7**(RK3576,6 TOPS NPU,4×A72+4×A53) |
| 屏幕 | 官方 5 寸 MIPI **F050008M01**(720×1280,GT9xx 电容触摸,I2C 0x5D) |
| 摄像头 | 官方 **IMX415**(800万,MIPI CSI,DTS 默认已启用 cam0/1/3) |
| 官方 SDK | `/home/olwhistle/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008` |
| SDK 状态 | ✅ 已解压(19GB,171206 文件),`git fsck --full` 全量校验通过,HEAD=`f06400239` |
| Armbian SDK | `Rk3576-SDK/Armbian/kickpi-armbian` 仅作参考,主线用官方 SDK |
| 编译机 | 同一台 Ubuntu VM:8 核 / 内存 7.2G(编译时 make -j 限 6)/ 磁盘 291G 可用 |
| 磁盘 | SATA SSD 直连(已通过 6GB×3 写读一致性认证)。**历史教训:USB 桥接时代大文件静默损坏**,大文件纪律见第六节 |
| 当前进度 | **B1~B4 完成**(2026-09-17 首个固件产出):依赖/分区烧录包见 `deliverables/firmware/20260917-B4/`;待办:B5 烧录验收(需板子)、wifibt 驱动修复(WiFi 暂不可用,用以太网)、recovery 镜像延后 |

---

## 二、产品定义

- 人脸识别门禁:刷脸(1:N)+ 活体检测(单目 RGB 动作指令式)→ 开锁
- 5 寸触摸屏显示:摄像头实时预览 + 识别结果 + 动作引导动画/文字
- 模块化预留:指纹模块(UART)、IC 读卡器(UART/SPI)后续接入,与人脸走同一认证抽象接口
- 系统:**Buildroot 无桌面 Linux**(LVGL 直跑 DRM,无 X11/Wayland),开发期即产品形态

---

## 三、总体架构

### 3.1 软件分层

```
┌─────────────────────────────────────────────────────┐
│  UI 层:LVGL(基于 SDK app/lvgl_demo 骨架)             │
│  界面:待机页/识别中/动作引导/结果页/管理页              │
├─────────────────────────────────────────────────────┤
│  业务服务层(线程 + 消息总线,互不直接调用)              │
│   capture_service   取流:rkaiq 3A + V4L2 → NV12 环形缓冲│
│   vision_service    ROCKIVA 检测/识别/1:N检索(官方方案)  │
│   liveness_service  动作状态机(随机序列)+姿态/张嘴判定   │
│   access_service    认证融合、门控决策、继电器、日志       │
│   enroll_service    人脸/指纹/卡注册管理                 │
├─────────────────────────────────────────────────────┤
│  HAL 层(薄封装,可替换)                                │
│   camera_hal  display_hal(DRM)  npu_hal(librknnrt)  │
│   gpio_hal(libgpiod)  uart_hal(指纹/读卡)  storage(SQLite)│
├─────────────────────────────────────────────────────┤
│  平台层:官方 SDK(kernel-6.1 + rkaiq + rknpu2 + rga    │
│          + mpp + libmali + buildroot rootfs)          │
└─────────────────────────────────────────────────────┘
```

### 3.2 数据流

```
IMX415 ─MIPI→ RKISP(rkaiq 3A)→ NV12 帧
   ├─① RGA 缩放 ─→ DRM video plane(屏幕预览,零拷贝)
   ├─② 人脸区域 → ROCKIVA(检测/关键点/识别/1:N 检索,NPU,官方方案)
   ├─③ 关键点序列 → liveness_service(动作判定)
   └─④ GStreamer → RTSP 推流(远程实时预览,可选)
比对/活体结果 → access_service → GPIO 继电器开锁 + SQLite 日志
事件总线 → LVGL UI 刷新(预览叠加框、结果、动作提示)
```

### 3.3 关键设计原则

1. **认证抽象**:`auth_provider` 统一接口(verify/enroll/abolish),face/finger/card 各自实现,access_service 只面向接口 —— 后接指纹和读卡器零改动。
2. **帧只拷一次**:ISP→RGA→NPU/RGA→DRM 全程零拷贝(drmPrime/dma-buf),CPU 不碰像素。
3. **UI 与视觉解耦**:UI 订阅事件总线,不直接调视觉模块;视觉不依赖 UI。
4. **模块独立成静态库**,main 只做装配,便于单元测试与替换。

### 3.4 应用代码目录(door-guard/)

```
door-guard/
├── app/            main.c、装配、配置加载
├── ui/             LVGL 界面(页面、主题、动作引导动画)
├── modules/
│   ├── capture/    取流线程、帧环形缓冲
│   ├── vision/     检测/识别/特征库比对
│   ├── liveness/   动作状态机、姿态判定
│   └── access/     门控决策、日志
├── hal/            camera/display/npu/gpio/uart/storage 薄封装
├── proto/          消息与事件定义(帧、事件、结果)
├── auth/           auth_provider 接口 + face/finger/card 实现
├── third_party/    lvgl、librknnrt 头+so、cjson、sqlite3
├── tools/          模型转换脚本(PC 端)、阈值标定脚本
└── configs/        device.json(分辨率/阈值/串口/GPIO 配置)
```

---

## 四、各模块技术要点

### 4.1 显示(LVGL + DRM)

- **LVGL 9.x**,直接跑 DRM:首版用 CPU 渲染(720×1280 单 UI 层,CPU 足够),后期可切 GPU。
- **双平面显示**:`video plane`(NV12,放摄像头预览)+ `UI plane`(ARGB8888, LVGL,叠在上层)——预览零拷贝,UI 半透明叠加,这是门禁机的标准做法。
- 触摸:内核 `goodix,gt9xx` 驱动(屏 dtsi 已含节点)→ `/dev/input/eventX` → LVGL evdev 输入。
- **屏幕使能(必须改 DTS)**:`rk3576-kickpi-k7-linux.dts` 当前未 include 任何屏,需加一行:
  `#include "rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi"`(文件已确认存在于 kernel-6.1 dts 目录,内含 GT9xx 触摸节点)。
- 骨架:`app/lvgl_demo`(SDK 自带,含 hal/ 和示例),以其为起点改造。

### 4.2 摄像头(IMX415)

- 通路:IMX415 → CSI → RKISP;3A 用 `external/camera_engine_rkaiq`,IQ 文件在
  `external/camera_engine_rkaiq/rkaiq/iqfiles/`(imx415_*.xml,按板端 ISP 版本选子目录,烧录后实测确认)。
- **首选底座:RKADK**(`app/rkadk`,Rockchip 应用开发套件):已封装 VI(取流)/VO(显示)/VENC/播放器,
  自带 `rkadk_stream_test / rkadk_disp_test / rkadk_ui_test` 等 15 个 example 和中英文开发指南 PDF。
  capture/display 两个 HAL 直接基于 RKADK 实现(内部已是 V4L2+DRM 零拷贝管线),省去裸写;不够用的层次再下沉到
  rockit MPI(`external/samples`,见 `sample_demo_vi_venc.c` 等)或裸 V4L2。
- 画质调优:`rkaiq_tool_server` + PC 端 IQ Tool 在线调曝光/色彩/降噪,导出 IQ xml 随固件发布。
- **远程推流(已定)**:GStreamer + RTSP 服务端(`BR2_PACKAGE_GST1_RTSP_SERVER=y` 已启用)。
  分工:上屏预览走 RKADK/RGA→DRM(§4.1),远程实时预览走 GStreamer RTSP 管线;
  两者共享取流源或各自开流,B8 阶段按 CPU 占用实测决定。

### 4.3 人脸识别:官方 ROCKIVA 方案(已定,2026-09-17)

- **采用 `external/iva` 的 ROCKIVA**(Rockchip 官方智能视觉分析 SDK):
  - 能力:人脸检测 + 关键点 + **1:N 人脸检索(注册/搜索)** + 属性(性别/年龄/表情/眼镜等);模型自带,NPU 推理
  - 板型适配:`librockiva/rockiva-rk3576-Linux`(预编译库)+ `models/rockiva_data_rk3576`
  - API:`include/rockiva_face_api.h`(初始化/送帧 + 人脸注册/检索接口)
  - 文档:`Rockchip_Developer_Guide_ROCKIVA_SDK_CN.pdf`(已拷入本仓库 sdk-guide/docs/)
  - Buildroot:`BR2_PACKAGE_IVA=y` + `BR2_PACKAGE_IVA_RK3576=y`(已启用,装 staging 可直接链接)
- **活体判定输入**:ROCKIVA 输出的关键点/姿态供 liveness_service 使用(张嘴判定二期接口部能力或补 landmark 模型)
- **备选/扩展**:需自定义识别模型或更高精度时,再走 rknn-toolkit2 + rknn_model_zoo 自组 SCRFD+ArcFace(转换方法见 sdk-guide §5);无论哪条路,比对阈值都要实测 ROC 标定
- 版本对齐:烧录后 `dmesg | grep rknpu` 查驱动版本

### 4.4 活体检测(单目 RGB,动作指令式)

- **状态机**:待机 → 检出人脸(稳定 N 帧)→ 下发随机动作序列(2~3 个,每个限时 1.5~2s)→ 逐项判定 → 全过则放行进入识别比对;任一超时/不符 → 失败重来。
- 动作集:点头 / 摇头 / 左转头 / 右转头 / 张嘴(+眨眼可选)。
- **判定器**:
  - 头部姿态:由 5 关键点几何估计 yaw/pitch(左右眼-鼻尖水平距比、鼻-眼垂直比),滑动窗口平滑;
    摇头=yaw 振荡过阈;左/右转=yaw 定向偏移;点头=pitch 先增后回。
  - 张嘴:需要嘴部 landmark(首版可用"框高变化+嘴部区域纹理变化"过渡,二期接 98 点 landmark rknn 模型)。
- **能力边界(明确写入产品需求)**:可防静态照片、基础屏幕重放(随机动作+限时);**不能**防高清屏定向重放、高仿真头模。若量产安全等级要求高,升级路径:IR 双目模组(rgb+ir 交叉活体),架构上 liveness_service 接口不变,换实现。
- 防体验作弊:动作顺序随机、限时、动作间强校验(未按指令做即失败)。

### 4.5 指纹模块(预留接口)

- 选型建议:UART 光学模组(AS608/ZFM 系,3.3V TTL,协议公开简单),识别在模组内完成,不占 NPU。
- 封装 `finger_provider: enroll(id)/verify()->(id,score)`;与人脸绑定同一 user_id。

### 4.6 IC 读卡器(预留接口)

- 选型建议:UART 串口读头(直接吐 UID,最省事)或 RC522(SPI,自写驱动);M1 卡读 UID+扇区校验可选。
- 封装 `card_provider: poll()->card_id`;卡号↔user_id 映射入库。

### 4.7 门控与存储

- 开锁:libgpiod 控继电器(脉冲 1~10s 可配);门磁反馈输入可选。
- 记录:SQLite(`BR2_PACKAGE_SQLITE=y` 已启用)存开门日志/告警/特征库;RTC + chrony(NTP)校时。

### 4.8 网络与联网(门禁必需,已核实)

- **以太网**:RK3576 GMAC 内核原生支持;DHCP 由 dhcpcd(`network.config` 已启用)
- **WiFi**:wifibt 片段已启用,作备选链路
- **时间同步**:chrony(NTP)——门禁日志强依赖正确时间,部署时必配 NTP 服务器
- **远程运维**:dropbear(SSH)+ SFTP(`network.config` 已启用)
- **管理面规划**:首版以 SSH + 配置文件交付;二期加局域网 Web 管理接口(人脸/卡/指纹库管理、日志查询)

---

## 五、固件编译计划(下次会话执行)

> 编译机 = 本 VM。SDK 顶层 `./build.sh`;板级 defconfig 位于
> `device/rockchip/.chips/rk3576/rockchip_rk3576_kickpi_k7_buildroot_defconfig`
> (K7 的 DTS:`rk3576-kickpi-k7-linux`;Buildroot 根配置:`buildroot/configs/rockchip_rk3576_kickpi_k7_doorGuard_defconfig`)。

| Phase | 内容 | 验收标准 |
|---|---|---|
| B1 | 环境自检:依赖包(git ssh make gcc 等按 SDK docs)、外网连通、磁盘/内存 | 自检脚本全绿 |
| B2 | **Buildroot 定制配置生效**:配置文件已建(SDK 分支 `k7-door-guard-dev`,补丁 `sdk-patches/0001+0002`):去 weston/chromium,加 lvgl(带 DRM 后端)/中文 locale/gdb+strace/dropbear,启用 `BR2_PACKAGE_RKADK`+`RKADK_USE_AIQ`、`BR2_PACKAGE_IVA`(RK3576,官方人脸)、`BR2_PACKAGE_GST1_RTSP_SERVER`(推流)、`BR2_PACKAGE_SQLITE`(数据库);B2 时确认 `RK_BUILDROOT_CFG` 绑定方式使 doorGuard 配置被选用 | `buildroot/output/*/` 下 menuconfig 可见 LVGL/RKADK/RKAIQ/IVA/SQLITE/GST1_RTSP_SERVER 开启、weston 关闭 |
| B3 | **屏幕使能**:K7 DTS 加 include 5 寸 dtsi;顺带确认触摸节点 | kernel 编过,dtb 里能反查 panel/gt9xx 节点 |
| B4 | 全量编译:`./build.sh` 选 K7 buildroot 配置 → uboot+kernel+rootfs+镜像(首次需外网拉依赖,预计 1~3h,`make -j6`) | `output/` 产出镜像;**镜像 md5 连读两次一致** |
| B5 | 烧录验证:USB(Maskrom/Loader)烧录;串口 1500000 8N1 + SSH(dropbear) | 验收清单:登录✓ dmesg 无异常✓ rknpu 驱动✓ media-ctl 有 IMX415 拓扑✓ 屏亮✓ 触摸有 event✓ |
| B6 | 摄像头出图:rkaiq 起 3A,v4l2 抓帧存图人工确认成像 | NV12 抓帧图正常曝光/色彩 |
| B7 | NPU 单项:ROCKIVA 人脸 demo 上板(检测/注册/1:N 检索) | 检测出框,检索返回正确 ID |
| B8 | 应用整合:door-guard 骨架 + LVGL 上屏 + 预览上屏 | 预览+UI 同屏,触摸可操作 |
| B9 | 人脸全链路:注册→识别→开门→日志 | 端到端 <1s,误识/拒识标定 |
| B10 | 活体接入 → 指纹/读卡器接入 → 产品化裁剪(去调试包/只读根文件系统/数据分区分离/签名) | 按各自验收 |

> 决策记录(2026-09-17):**直接采用 Buildroot 作为唯一 rootfs 路线**(放弃 Ubuntu 过渡方案)。
> 依据:Buildroot 片段体系完整覆盖本项目全部需求——`lvgl.config`(自带 DRM 后端)、
> `multimedia/camera.config`(rkaiq)、`npu2.config`、`gpu/gpu.config`、`network/network.config`
> (dropbear)、`tools/gdb.config`,RKADK 包自动拉起 rockit/rkaiq 视觉链。
> 用户有开发基础,接受"缺包改配置重编 rootfs"的迭代方式,换取从第一天就是产品形态。

---

## 六、工程纪律(本机踩坑固化)

1. **大文件双读校验**:任何 >1GB 的产物(镜像/压缩包),`md5sum` 连读两次一致才算有效——本机 USB 时代曾 5 读 5 值,烧坏镜像排查代价极高。
2. **大文件传输用流式**(tar 管道/直接解压),不做"先拷包再解压"的中转。
3. **若再出现文件异常**:第一时间跑 `sudo memtester 4G 1` 排查内存(此疑点未最终排除),再查盘。
4. 编译并发 `make -j6`(内存 7.2G,防 OOM)。
5. SDK 改动(git 管理):rootfs 脚本、DTS 的修改都在 git 分支上做,可回退可 diff。
6. 烧录线/USB 口固定一套,烧录失败先查线再查板。

---

## 七、风险登记

| 风险 | 等级 | 对策 |
|---|---|---|
| Buildroot 缺包/配置遗漏导致迭代重编 | 中(已接受) | doorGuard defconfig 一次配全常用工具;缺啥补 defconfig 重编 rootfs;复杂探索性工作放 PC 交叉环境 |
| 单目 RGB 活体防不住高级重放 | 中(产品定位相关) | 已声明边界;预留 IR 双目升级路径,liveness 接口不变 |
| i8 量化后识别阈值漂移 | 中 | 量化后必做 ROC 标定,阈值进配置文件 |
| 编译机内存 7.2G 偏小 | 低 | -j6;必要时加 swap |
| 触摸/屏幕排线硬件差异 | 低 | dtsi 与官方屏一一对应;不亮先查排线 |
| IMX415 IQ 效果(色偏/噪点) | 低 | rkaiq IQ xml 可调;先默认后调优 |
