# 固件包 20260917-B4(K7 Buildroot 无桌面)

第一个自编译固件。Buildroot rootfs(doorGuard 定制:LVGL+DRM 后端、ROCKIVA、RKADK+RKAIQ、
rknpu2、GStreamer+RTSP、SQLite、dropbear/chrony,无桌面无 X11)。

## 两种烧录方式

1. **一键烧录**:RKDevTool"升级固件"页,选 `update.img`(已含全部分区;recovery 分区为空)
2. **分区烧录**:RKDevTool"下载镜像"页,按下方地址表逐个载入(改哪刷哪,推荐日常迭代用)

## 校验

烧录前必做:`md5sum -c md5sums.txt`

## 分区烧录地址(RKDevTool"下载镜像"模式,单位:扇区)

| 文件 | 地址 | 说明 |
|---|---|---|
| MiniLoaderAll.bin | 0x0 | Loader(工具自行放置 IDB) |
| parameter.txt | 0x0 | 分区表,**地址固定填 0x0**,工具特殊处理、交给设备重建 GPT |
| uboot.img | 0x4000 | U-Boot |
| misc.img | 0x6000 | 启动模式标记 |
| boot.img | 0x8000 | 内核 6.1 + 设备树(FIT,含 5 寸屏 F050008M01 使能) |
| rootfs.img | 0x78000 | Buildroot rootfs(可增长分区) |

> ⚠️ **IDB/Loader 保留区**:eMMC 0x4000 扇区(8MB)之前是 BootROM/Loader 保留区,
> 除 Loader 行与 parameter 行(均填 0x0,工具特殊处理)外,任何镜像不得写到该区域
> (实测 RKDevTool 对 parameter 填 0x800 会拦截:"IDB 将会被 parameter.txt 破坏,
> 不要写数据在 4824 扇区之前")。若 parameter 行在 0x0 仍触发该警告,改用
> upgrade_tool `di -p` 方式,见 docs/tech/FLASHING.md。一键烧录 `update.img` 不涉及此问题。

## 本版已知事项

1. **recovery 分区未包含**(恢复模式镜像构建耗时长,已延后;不影响正常启动与功能)
2. **板载 WiFi(SWT6621S)驱动编译失败**(厂家 wifibt 构建脚本头文件拷贝顺序 bug),
   本版 WiFi 不可用,**联网请用以太网口**;修复已在队列(补拷头文件后重编)
3. rootfs 内预置:dropbear(SSH)/chrony(NTP)/gdb 调试链 —— 产品化时按
   PROJECT_PLAN §4.3 doorGuard 配置注释裁剪

## 首次开机验收

按仓库 `docs/tech/FLASHING.md` 的 B5 清单逐项检查。

## 二进制不进 git(2026-09-18)

镜像与 Loader(update/rootfs/boot/uboot/misc.img、MiniLoaderAll.bin)自 2026-09-18 起
不再进 git 跟踪(.gitignore 排除):GitHub 单文件上限 100MB 拒收,且源码仓不应携带
大产物。本目录 md5sums.txt/parameter.txt 等文本保留;镜像本体只存本地 + 局域网
Gitea(gitea remote 的历史里含全量)。换机时从 Gitea 或本地盘取镜像。
