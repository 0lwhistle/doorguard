# sdk-patches

对官方 SDK 的全部修改以 patch 形式存放于此(现 8 个,0001~0008):

| 补丁 | 说明 |
|---|---|
| `0001-buildroot-K7-doorGuard.patch` | buildroot:新增 K7 doorGuard defconfig(基础裁剪与工具) |
| `0002-buildroot-doorGuard-IVA-GST-RTSP-SQLite.patch` | buildroot:doorGuard defconfig 增补 IVA/GST1_RTSP_SERVER/SQLite 等包启用 |
| `0003-device-kernel-doorGuard-buildroot-K7-5-MIPI.patch` | device:板级 defconfig 绑 doorGuard 配置;kernel:K7 DTS include 5 寸 MIPI 屏 dtsi |
| `0004-buildroot-HOST_GDB_PYTHON-legacy.patch` | buildroot:defconfig 显式设 HOST_GDB_PYTHON(旧式路径兼容) |
| `0005-buildroot-host-cmake-GCC15-cmcppdap-cstdint.patch` | buildroot:host-cmake 打 cmcppdap cstdint 补丁(GCC15 编译适配) |
| `0006-buildroot-rkadk-.git-version.h.patch` | buildroot:rkadk 包补丁,version.h 强制生成(免 .git 依赖) |
| `0007-rkadk-version.h-buildroot-local.patch` | rkadk:Version.cmake + 包补丁配套,version.h 无条件生成为 buildroot 本地版 |
| `0008-rkadk-configure_file-if-EXISTS-.git-version.h.patch` | rkadk:configure_file 对 .git/version.h 加 IF EXISTS 守卫(本地包不误判) |

生成(在 VM 的 SDK 仓库,分支 k7-door-guard-dev):
    git format-patch f06400239 -o ~/Linux/rk3576/k7_rk3576/sdk-patches/

复原(新环境):在 SDK 仓库对应分支上 `git am` 应用——所有补丁均落在 SDK 单仓
(buildroot/kernel-6.1/device/app/rkadk),SDK 顶层 `git am sdk-patches/*.patch` 即可按序复原。
