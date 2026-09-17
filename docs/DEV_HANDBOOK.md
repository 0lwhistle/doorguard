# K7 门禁项目 — 软件开发手册

> 面向:在本项目上做开发的任何人(WSL 侧 / VM 侧)。
> 上次更新:2026-09-17(固件 20260917-B4 产出后)。
> 配套文档:`PROJECT_PLAN.md`(方案与里程碑)、`sdk-guide/README.md`(SDK 资源)、`docs/tech/FLASHING.md`(烧录)、`docs/tech/TOOLCHAIN.md`(WSL 交叉编译)、`docs/DEVLOG.md`(开发日志)。

---

## 1. 项目速览

- **目标**:RK3576 人脸识别门禁(LVGL 界面 + 官方 ROCKIVA 人脸识别 + 动作活体 + 预留指纹/IC 卡)
- **硬件**:KickPi K7(RK3576,6T NPU)+ 5 寸 MIPI 屏 F050008M01(720×1280,GT9xx 触摸)+ 官方 IMX415 摄像头
- **系统**:Buildroot 无桌面 Linux(内核 6.1,vendor 分支),LVGL 直跑 DRM,无 X11/Wayland
- **技术栈已定**:UI=LVGL;取流/上屏=RKADK/RGA→DRM;远程推流=GStreamer RTSP;人脸=ROCKIVA;数据库=SQLite;联网=以太网(主)

---

## 2. 硬件事实(实测确认)

| 项 | 结论 |
|---|---|
| 摄像头接口 | **插在 cam2 口**(I2C 总线 8,地址 0x37,驱动实体名 `m02_b_imx415 8-0037`);cam0/cam1 口未插(探测 ID=0 属正常) |
| IMX415 输出 | 3864×2192 **RAW10**,4 lane,驱动版本 00.01.08;已确认在出流(曝光寄存器在写) |
| DPHY | `csi2-dphy3` 与 cam2 的 IMX415 配对 |
| 屏 | F050008M01 已点亮,DTS 已 include 屏 dtsi(PWM 背光 + GT9xx 触摸 I2C) |
| 触摸 | `/proc/bus/input/devices` 中 goodix,屏幕+触摸+LVGL demo 实测正常 |
| WiFi/BT | SWT6621S(SDIO);**驱动编译失败(已知问题,见 §8),当前联网用以太网** |
| 以太网 | GMAC 内核原生,正常 |
| 串口调试 | **1500000 8N1**(不是 115200) |

---

## 3. 当前固件(20260917-B4)

- 位置:仓库 `deliverables/firmware/20260917-B4/`(update.img 一键包 + 分区五件套 + parameter.txt + md5sums)
- **烧录前必做**:`md5sum -c md5sums.txt`
- 烧录方式:①RKDevTool"升级固件"选 update.img;②"下载镜像"按地址表(见包内 README)——**改 DTS 后只需刷 boot 分区**
- **本版已知缺口**:
  1. recovery 分区为空(构建耗时长已延后)
  2. 板载 WiFi 驱动缺失(厂家 wifibt 脚本 bug,见 §8;联网用以太网)
- rootfs 内已预置:LVGL(+demo 自启,后续换 door-guard)、ROCKIVA(含模型)、RKADK、rkaiq、rknpu2、GStreamer+RTSP server、SQLite、dropbear、chrony、gdb 调试链

### B5 验收状态

| 项 | 状态 |
|---|---|
| 启动/串口/登录 | ✅ |
| 屏幕点亮 + 触摸 | ✅(LVGL demo 自启验证) |
| IMX415 探测/出流 | ✅(cam2, 3864×2192 RAW10) |
| ISP 节点定位 / 抓 NV12 帧 | ⏳ 进行中(见 §5) |
| rknpu 驱动确认 | ⏳ |
| 以太网 / SSH | ⏳ |

---

## 4. 板端速查

```bash
dmesg | grep -i imx415        # 传感器探测/出流状态
ls /dev/video* /dev/media*    # 媒体节点(media0~8:多个 rkcif 实例 + ISP 等)
for m in /dev/media*; do echo "== $m"; media-ctl -d $m -p 2>/dev/null | grep -E "^driver|imx415|rkisp" | head -3; done
dmesg | grep -i rknpu         # NPU 驱动
ip a; ps | grep dropbear      # 网络/SSH
cat /sys/class/backlight/*/brightness   # 背光
cat /proc/bus/input/devices | grep -iA3 goodix  # 触摸
```

注意:rootfs 裁剪过,缺什么工具优先想"buildroot defconfig 里没开",回 VM 加配置重编,不要板端乱装。

---

## 5. 摄像头子系统现状(重要,接手必读)

- 板上有 **8 个 /dev/mediaX**,是 RK3576 的多 rkcif 实例 + ISP 等混排,**别假设 media0 就是摄像头**
- IMX415 走 **dphy3**,对应哪个 rkcif 实例/ISP 链路、哪个 /dev/videoX 出 NV12,**尚未最终定位**(映射命令见 §4 第一条,拿到结果后更新本节)
- 两条取流路径:
  - **rkcif 直采**:RAW10 裸帧(无 3A,画面偏暗/偏绿),仅用于验证传感器出图
  - **rkisp + rkaiq 3A**:正式成像路径,输出 NV12,门禁用这条;3A 服务(rkaiq)需先起
- B6 验收:抓到 1920×1080 NV12(3.1MB/帧)+ 帧内容亮度方差正常

---

## 6. 编译环境(编译 VM)

| 项 | 值 |
|---|---|
| SDK 路径 | `~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008` |
| SDK 分支 | `k7-door-guard-dev`(基于厂家 HEAD `f06400239`) |
| 设备 defconfig | `rockchip_rk3576_kickpi_k7_buildroot_defconfig`(已绑 doorGuard) |
| Buildroot 配置 | `buildroot/configs/rockchip_rk3576_kickpi_k7_doorGuard_defconfig` |
| Buildroot 输出 | `buildroot/output/rockchip_rk3576_kickpi_k7_doorGuard/` |
| 产物 | `output/firmware/`(分区镜像);update.img 用 `RK_UPDATE=y ./build.sh firmware` 打包 |

### 常用命令

```bash
./build.sh                      # 全量(all,断点续建,已完成阶段秒过)
./build.sh kernel               # 只编内核(改 DTS 后用,增量分钟级)
./build.sh firmware             # 打包分区镜像
RK_UPDATE=y ./build.sh firmware # 打包 update.img
./build.sh rootfs               # 只编 rootfs(改 buildroot 配置后用)
```

### 环境适配点(全部已固化,重装系统/换机时照做)

1. **python2 垫片**:`/usr/local/bin/python2` → `#!/bin/sh exec /usr/bin/python3 "$@"`(u-boot 检查需要)
2. **SDK 归属**:`chown -R olwhistle:olwhistle <SDK>`(构建系统内核阶段会 `sudo -u #1000` 降权,root 属主会 Permission denied)
3. **gcc-13 影子**:`/usr/local/bin/gcc|g++ → gcc-13|g++-13`(buildroot HOSTCC 硬编码 `gcc`,而系统 GCC 15 对 host 包过严)
4. **`FORCE_UNSAFE_CONFIGURE=1`**:每次 `./build.sh` 前带(host-tar/mtools 拒绝 root)
5. **swap**:/swapfile-4g(4G,重启后需 `swapon /swapfile-4g`,内存 7.2G 防编译 OOM)
6. **依赖包**:libgmp-dev libmpfr-dev libmpc-dev libelf-dev 等(apt 装过)

### SDK 修改管理(纪律)

- 一切 SDK 改动提交在 `k7-door-guard-dev` 分支
- 每次修改后:`git format-patch f06400239 -o <repo>/sdk-patches/` 导出补丁进本仓库
- 目前补丁:**0001** doorGuard buildroot 配置;**0002** IVA/RTSP/SQLite;**0003** doorGuard 绑定+屏 dtsi;**0004** HOST_GDB_PYTHON 废弃规避;**0005** host-cmake GCC15 补丁;**0006** HOST_GDB_PYTHON 遗留补丁清理;**0007** rkadk version.h 无条件生成

### 踩过的坑(按报错特征索引)

| 报错特征 | 原因 | 解法 |
|---|---|---|
| `Your python2 is missing` | u-boot 检查 | python2 垫片(见上 1) |
| `Permission denied` 写内核目录 | 构建降权 uid1000 | chown SDK(见上 2) |
| `gmp/mpc header is missing` | 内核宿主工具 | apt 装 dev 包(见上 6) |
| `legacy configuration in .config` | HOST_GDB_PYTHON 已废弃 | 补丁 0004(显式 unset) |
| host-cmake `uint32_t has not been declared` | GCC15 + cmcppdap | 补丁 0005 |
| host-m4 gl_xlist 隐式声明 | GCC15 过严 | gcc-13 影子(见上 3)+ 删旧 build 目录 |
| host-tar `should not run configure as root` | root 构建 | FORCE_UNSAFE_CONFIGURE=1 |
| rkadk `version.h` 找不到 | 无 .git 不生成 | 补丁 0007 |
| 系统内存持续 100%,shell 都 fork 不出 | buildroot -j 自动打满 | 正常现象,等;或加 swap |

---

## 7. 开发工作流

- **代码流转只经 git**:WSL(写代码/单测)→ push → Gitee(`ssh://git@192.168.2.150:222/olwhistle/k7_rk3576.git`)→ VM pull 交叉编译
- **WSL 快速循环**(door-guard 应用):`source env/env.sh` → `dg-build` → `dg-deploy -r <板子IP>`;
  工具链安装/更新 `dg-tc-install`;技术细节见 `docs/tech/TOOLCHAIN.md`
- SDK(19GB)**只在 VM 存一份**;SDK 改动走分支+补丁(见 §6)
- 板端部署:scp 可执行文件 / buildroot 包;固件迭代走"增量编译 → 只刷对应分区"
- VM 大文件纪律:任何 >1GB 产物 md5 连读两次一致才算有效;传输用流式
- **sysroot 重导出注意**:`buildroot/output/<cfg>/staging` 是**绝对路径软链**(指向 host/…/sysroot),
  直接 `tar czf … staging` 只会打进链接本身(WSL 解压仅 276 字节)。必须:
  `tar czfh doorguard-sysroot.tar.gz --exclude='./dev' -C <output>/<cfg>/host/aarch64-buildroot-linux-gnu/sysroot .`

---

## 8. 已知问题与待办

| 问题 | 状态 | 计划 |
|---|---|---|
| SWT6621S WiFi 驱动编译失败(`skw_platform_data.h` 未拷入内核) | 未修 | 厂家脚本加"先拷头文件"步骤,分支补丁;下一版固件恢复 WiFi |
| recovery 镜像未构建(独立 buildroot 全量,耗时 1h+) | 延后 | 空闲时段补;不阻塞功能 |
| LVGL demo 自启占用屏幕 | 未处理 | door-guard 应用就位后替换自启 |
| ISP 节点定位 / rkaiq 3A 起流 | 进行中(B6) | 完成 §5 映射后抓 NV12 |
| ROCKIVA 上板验证 | 待 B7 | 库+模型已在 rootfs |
