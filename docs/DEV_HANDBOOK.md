# K7 门禁项目 — 软件开发手册

> 面向:在本项目上做开发的任何人(WSL 侧 / VM 侧)。
> 上次更新:2026-09-17(固件 20260917-B4 产出后)。
> 配套文档:`PROJECT_PLAN.md`(方案与里程碑)、`sdk-guide/README.md`(SDK 资源)、`docs/tech/FLASHING.md`(烧录)、`docs/tech/TOOLCHAIN.md`(WSL 交叉编译)、`docs/DEVLOG.md`(开发日志)。

---

## 1. 项目速览

- **目标**:RK3576 人脸识别门禁(LVGL 界面 + 官方 ROCKIVA 人脸识别 + 动作活体 + 预留指纹/IC 卡)
- **硬件**:KickPi K7(RK3576,6T NPU)+ 5 寸 MIPI 屏 F050008M01(720×1280,触摸 IC 随屏组装为 FocalTech/GT9xx)+ 官方 IMX415 摄像头
- **系统**:Buildroot 无桌面 Linux(内核 6.1,vendor 分支),LVGL 直跑 DRM,无 X11/Wayland
- **技术栈已定**:UI=LVGL;取流/上屏=RKADK/RGA→DRM;远程推流=GStreamer RTSP;人脸=ROCKIVA;数据库=SQLite;联网=以太网(主)

---

## 2. 硬件事实(实测确认)

| 项 | 结论 |
|---|---|
| 摄像头接口 | **插在 cam2 口**(I2C 总线 8,地址 0x37,驱动实体名 `m02_b_imx415 8-0037`);cam0/cam1 口未插(探测 ID=0 属正常) |
| IMX415 输出 | 3864×2192 **RAW10**,4 lane,驱动版本 00.01.08;已确认在出流(曝光寄存器在写) |
| DPHY | `csi2-dphy3` 与 cam2 的 IMX415 配对 |
| 屏 | F050008M01 已点亮,DTS 已 include 屏 dtsi(PWM 背光 + 触摸 I2C) |
| 触摸 | **IC 随屏组装不同**:当前屏 FocalTech `fts_ts`(I2C0-0038→event1,Type-B MT);早期屏为 GT9xx@0x5D。DTS 两驱动共存自动适配;应用经 touch_evdev 自动探测(2026-09-18 实测) |
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

- **网络**:默认 IP **192.168.2.95**(DHCP;变了改 env/env.sh 的 DOORGUARD_IP)
- **SSH**:`root` / 密码 `a231634904`(开发板局域网口令;WSL 公钥已装,`dg-deploy` 免密)
- 串口 **1500000 8N1**(不是 115200);dropbear 拒绝空密码

```bash
dmesg | grep -i imx415        # 传感器探测/出流状态
ls /dev/video* /dev/media*    # 媒体节点(media0~8:多个 rkcif 实例 + ISP 等)
for m in /dev/media*; do echo "== $m"; media-ctl -d $m -p 2>/dev/null | grep -E "^driver|imx415|rkisp" | head -3; done
dmesg | grep -i rknpu         # NPU 驱动
ip a; ps | grep dropbear      # 网络/SSH
cat /sys/class/backlight/*/brightness   # 背光
cat /proc/bus/input/devices | grep -iA3 'fts\|goodix'  # 触摸(看实际枚举出的 IC)
/etc/init.d/S60doorguard {start|stop|restart}  # 应用自启服务;日志 /var/log/door-guard.log
```

注意:rootfs 裁剪过,缺什么工具优先想"buildroot defconfig 里没开",回 VM 加配置重编,不要板端乱装。

---

## 5. 摄像头子系统现状(重要,接手必读)

- 板上有 **8 个 /dev/mediaX**,是 RK3576 的多 rkcif 实例 + ISP 等混排,**别假设 media0 就是摄像头**
- **已定位(2026-09-18 B6)**:IMX415(cam2 口,实体名 `m02_b_imx415 8-0037`)→ rkcif → **rkisp-vir2 = /dev/media5**,mainpath = **/dev/video51**;实体名查法 `cat /sys/class/video4linux/v4l-subdev*/name`
- 两条取流路径:
  - **rkcif 直采**:RAW10 裸帧(无 3A),仅用于验证传感器出图
  - **rkisp + rkaiq 3A**:正式成像路径,**已打通**(door-guard 预览在用):V4L2 单平面 NV12 1280x720 → RGA 旋转90+转 XRGB → LVGL。⚠️ 3 个死坑:uAPI2 参数是传感器实体名(非 media 节点,传错段错误);aiq2.lock 死锁需"取流线程与 prepare 并发会合";librga 成功码有两个——详见 door-guard/hal/camera/README.md
- door-guard 相机链路开关与环境变量见 `door-guard/hal/camera/README.md`

---

## 5.5 WSL 宿主开发依赖(闲时任务新增,2026-09-18)

| 依赖 | 用途 | 安装 |
|---|---|---|
| libsdl2-dev | PC 模拟器显示(已有) | apt |
| libsqlite3-dev | 宿主跑 storage 单测 | apt(本次安装) |
| libssl-dev | 宿主跑加密单测(已有) | apt |
| sqlite3 CLI | schema/数据校验 | apt(本次安装) |
| xdotool | 模拟器交互走查(合成点击) | apt(本次安装;LVGL 30ms 采样会丢快点击,分步操作) |
| node/npx + lv_font_conv | 重新生成中文字体(门禁标签变更后跑 ui/font/gen.sh;生成产物已入库,日常无需 node) | nvm 已有 |
| 字体源 | /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf(CJK)+ DejaVuSans(Latin);**Droid 无 ASCII 字形,必须双字体** | 系统自带 |

## 6. 编译环境(编译 VM)

| 项 | 值 |
|---|---|
| SDK 路径 | `~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008` |
| SDK 分支 | `k7-door-guard-dev`(基于厂家 HEAD `f06400239`) |
| 设备 defconfig | `rockchip_rk3576_kickpi_k7_buildroot_defconfig`(已绑 doorGuard) |
| Buildroot 配置 | `buildroot/configs/rockchip_rk3576_kickpi_k7_doorGuard_defconfig` |
| Buildroot 输出 | `buildroot/output/rockchip_rk3576_kickpi_k7_doorGuard/` |
| 产物 | `output/firmware/`(分区镜像);update.img 用 `RK_UPDATE=y ./build.sh firmware` 打包 |
| ESP32 模板(复用 tasker/event_bus/holder) | `/home/olwhistle/dockerNow/esp32/programs/ovs`(WSL 本地;映射表见 skill references/architecture.md) |
| 中枢仓库 | **origin** = GitHub `git@github.com:0lwhistle/doorguard.git`(2026-09-18 起,源码主仓);**gitea** = `ssh://git@192.168.2.150:222/olwhistle/k7_rk3576.git`(历史归档,唯一含固件/工具链大文件的远程,勿 force push 覆盖) |

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

- **代码流转只经 git**:WSL(写代码/单测)→ push → **GitHub origin**(`git@github.com:0lwhistle/doorguard.git`)→ VM pull 交叉编译
- ⚠️ **Gitea 已停更(2026-09-18 历史重写)**:为上 GitHub,固件/工具链大 blob 已从历史剥离,
  本地与 Gitea 历史分叉。**VM 勿直接 pull gitea**(会撞回旧历史);等 VM 能直连 GitHub 后
  `git fetch origin && git reset --hard origin/master` 一次性切换。Gitea 旧历史是固件镜像
  (update/rootfs/boot.img 等)唯一异地副本,保留勿覆盖;换机取镜像从 Gitea 或本地盘
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
| LVGL demo 自启占用屏幕(/etc/init.d/S00-lv_demo) | **已禁用**(板上运行 door-guard 前提;mv 为 disabled-S00-lv_demo) | B10 固件收编为 door-guard 自启 |
| ISP 节点定位 / rkaiq 3A 起流 | 进行中(B6) | 完成 §5 映射后抓 NV12 |
| ROCKIVA 上板验证 | 待 B7 | 库+模型已在 rootfs |
| ~~触摸输入:无输入节点~~ **已解决(2026-09-18)**:touch_evdev 接入,当前屏 fts_ts 工作正常;仅剩方向/灵敏度人工校验(偏转配 DG_TOUCH_* env) | 已闭环 | — |
| 门控 GPIO:继电器引脚未确认(勿在未知引脚写 direction,已致板挂起一次) | 待硬件确认 | 引脚确认后 gpio_hal 对拍 |
| /dev/fb0(rockchipdrmfb)mmap EBUSY | 已绕行 | 显示走 DRM dumb-buffer(lv_drivers) |
