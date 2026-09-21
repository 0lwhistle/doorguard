# 固件烧录说明(KickPi K7)

> 固件包:见 `deliverables/firmware/`(分区镜像 + 一键烧录 `update.img` + `md5sums.txt`)。
> 烧录前必做:固件包目录下 `md5sum -c md5sums.txt` 校验通过才能烧。

## 重要:IDB/Loader 保留区(分区烧录前先读这个)

eMMC **0x4000 扇区(8MB)之前**是 BootROM/Loader(IDB)保留区。分区烧录时只有两行
允许落在里面,且地址都必须填 **0x0**(工具特殊处理,不裸写扇区):

- Loader 行:`MiniLoaderAll.bin`
- parameter 行:`parameter.txt`(交给设备按它重建 GPT,**不可跳过**——自编译固件
  分区布局与厂家原厂不同,必须重建分区表,否则内核按分区索引找不到 rootfs)

其它任何镜像写到该区域都会烧坏 Loader(实测 RKDevTool 对 parameter 填 0x800 会报
"不要写数据在 4824 扇区之前")。**一键烧录 `update.img` 不涉及此问题。**

## 方式一:RKDevTool 一键烧录(推荐,首次烧录/整机更新)

1. 安装 Rockchip 官方 RKDevTool 和 Rockchip USB 驱动(DriverAssitant)
2. 按住板子 Recovery/BOOT 键(或短接 Maskrom 孔)上电/复位,进 Loader 或 Maskrom 模式
   - 设备管理器出现 "Rockusb Device" 即成功
3. RKDevTool → "升级固件" 页 → "固件" 选择 `update.img`(已含全部分区,
   recovery 分区为空)→ "升级"
4. 进度走完显示 "升级成功",板子自动重启

## 方式二:RKDevTool 分区烧录(日常迭代,改哪刷哪)

"下载镜像"页逐行添加并勾选(新版工具可点"导入配置"选 `parameter.txt` 自动填表,
注意核对地址):

| 地址 | 文件 |
|---|---|
| 0x0 | MiniLoaderAll.bin |
| 0x0 | parameter.txt |
| 0x4000 | uboot.img |
| 0x6000 | misc.img |
| 0x8000 | boot.img |
| 0x78000 | rootfs.img |

recovery/backup 本版无镜像,不勾。若 parameter 行(0x0)仍触发 IDB 警告,改走方式三。

## 方式三:Linux 端 upgrade_tool(VM 内可用,SDK 自带 tools/linux/Linux_Upgrade_Tool/)

```bash
# 整包:
sudo upgrade_tool uf update.img          # Loader/Maskrom 模式下整卡升级

# 分区烧录(di -p 把 parameter 作为元数据交给设备生成 GPT,可避开 IDB 保留区):
sudo upgrade_tool ul MiniLoaderAll.bin
sudo upgrade_tool di -p parameter.txt
sudo upgrade_tool di -u uboot.img -m misc.img -b boot.img -r rootfs.img
sudo upgrade_tool rd                     # 复位重启

# 单独更新某分区(如改内核后只刷 boot),地址见 parameter.txt:
sudo upgrade_tool wl 0x8000 boot.img
```

## 方式四:SD 卡(可选)

用 SDDiskTool 或 `dd` 把 `update.img` 写入 TF 卡,插卡从 SD 启动(量产以 eMMC 为准)。

## 救砖

BootROM 的 Maskrom 模式不依赖 eMMC 内容:短接 Maskrom 孔/按键进 Maskrom 后,
按上述任一方式重刷全部分区即可恢复,不会变砖。

## 串口调试

- 参数:**1500000 8N1**(注意波特率不是 115200)
- 默认账号:root(密码以厂家文档为准)/ 或 kickpi

## 首次开机验收清单(PROJECT_PLAN B5)

- [ ] 串口能登录,dmesg 无持续报错
- [ ] `dmesg | grep rknpu` 有 NPU 驱动加载记录
- [ ] `ls /dev/video*` 存在;`media-ctl -p` 能看到 imx415 链路
- [ ] 5 寸屏点亮背光
- [ ] `cat /proc/bus/input/devices` 有触摸设备(goodix 或 fts/focaltech,随屏装配)
- [ ] 网口 link up,dhcp 拿到 IP;`ping` 外网通
- [ ] dropbear SSH 可登录
