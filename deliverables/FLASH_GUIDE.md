# 固件烧录说明(KickPi K7)

> 固件包:见本目录 `firmware/`(每版固件含 `*.img` 镜像与 `md5sums.txt`)。
> 烧录前必做:`md5sum -c md5sums.txt` 校验通过才能烧。

## 方式一:Windows 端 RKDevTool(推荐,图形界面)

1. 安装 Rockchip 官方 RKDevTool 和 Rockchip USB 驱动(DriverAssitant)
2. 按住板子 Recovery/BOOT 键(或短接 Maskrom 孔)的同时上电/复位,进 Loader 或 Maskrom 模式
   - 设备管理器出现 "Rockusb Device" 即成功
3. RKDevTool → "升级固件" 页 → "固件" 选择 `update.img` → "升级"
4. 进度走完显示 "升级成功",板子自动重启

## 方式二:Linux 端 upgrade_tool(VM 内可用)

```bash
# SDK 自带:tools/linux/Linux_Upgrade_Tool/upgrade_tool
sudo upgrade_tool uf update.img          # Loader/Maskrom 模式下整卡升级
# 或分区级升级(改 DTS 后只需刷 boot):
sudo upgrade_tool wl 0x8000 boot.img     # 具体分区地址见 parameter.txt
```

## 方式三:SD 卡(如需)

用 SDDiskTool 或 `dd` 把 `update.img` 写入 TF 卡,插卡从 SD 启动(量产以 eMMC 为准)。

## 串口调试

- 参数:**1500000 8N1**(注意波特率不是 115200)
- 默认账号:root(密码以厂家文档为准)/ 或 kickpi

## 首次开机验收清单(PROJECT_PLAN B5)

- [ ] 串口能登录,dmesg 无持续报错
- [ ] `dmesg | grep rknpu` 有 NPU 驱动加载记录
- [ ] `ls /dev/video*` 存在;`media-ctl -p` 能看到 imx415 链路
- [ ] 5 寸屏点亮背光
- [ ] `cat /proc/bus/input/devices` 有 goodix 触摸设备
- [ ] 网口 link up,dhcp 拿到 IP;`ping` 外网通
- [ ] dropbear SSH 可登录
