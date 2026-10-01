# 硬件接入:VM 编译 + 驱动任务 交接单(指纹 AS608 + IC 卡读头)

> 日期:2026-09-30。目标:**一次 VM 内核编译 + 一次刷 boot**,同时满足指纹链路
> (时钟修复)与 IC 卡链路(SPI 启用 + 驱动挂载点)的全部内核侧需求。
> rootfs 不动。应用侧代码已全部就位(§1),刷机后按 §6 验收。
> 相关文档:`FINGERPRINT_PROTOCOL.md`(指纹协议 v1 冻结)、
> `FINGERPRINT_AS608.md`(业务方案)、`ICCARD_PROTOCOL.md`(IC 卡驱动-应用契约,
> **驱动作者必读**)、`FLASHING.md`(烧录)、`DEV_HANDBOOK.md §2/§4`(环境)。

## 0. 结论速览

| 事项 | 内核/VM 侧要做的 | 优先级 |
|---|---|---|
| 指纹 57600 不通 | DTS:uart8 assigned 时钟 24MHz(§2.1,**实测根因**) | **必须** |
| IC 卡驱动挂载 | DTS:启用 SPI 控制器 + dg_iccard 子节点(§2.2) | 驱动接入时必须 |
| IC 卡驱动本体 | 用户写 .ko(§3,契约已冻结) | 随 DTS 一起交付 |
| 指纹驱动 | **无内核驱动**(纯应用层 UART) | 无 |

## 1. 应用侧现状(无需 VM 侧操心,刷机即生效)
> **2026-10-01 更新:应用层全链已落地**——fp_provider(WAK→采集序列→事件)、
> card_provider(/dev/dg_iccard0 线程)、FSM 指纹/IC 分支、录入编排、UI 全部就位
> (services/verify/{fingerprint,ic}/README.md)。模组/驱动没上时两路 provider
> 自动降级(EV_SYS_SERVICE_STATE + 退避重试),整机照常——刷不刷机都不怕。

- 协议:AS608 帧协议已按官方 51 例程逐字节冻结(`FINGERPRINT_PROTOCOL.md`),
  组包/解析纯函数层 `services/verify/fingerprint/fp_as608.c` + 单测(官方 golden)。
- 存储:fingerprints 独立表 + users.finger_vec 幂等迁移已落(storage.c)。
- 配置:`configs/default.json` 新增 `"finger"` 组:`uart_dev=/dev/ttyS8`、
  `baud=57600`、`wak_gpio=94`(2026-09-30 板上实测定值)、`wak_active_level=1`(极性开放项)。
- `configs/default.json` `"iccard"` 组:`dev_path=/dev/dg_iccard0`(驱动最终定名后只改配置不改码);"sim"=宿主模拟后端。
- 硬件已接线并核验:AS608 TXD/RXD ↔ uart8(ttyS8,status=okay、pinctrl default、
  无占用)、WAK → **GPIO2_D6**(sysfs 号 94,MUX/GPIO 双未占用)、VTI → 3.3V。

## 2. 设备树需求(VM SDK 板级 dts)

### 2.1 uart8 时钟修复(指纹阻塞项,已实测定位)

**现象**:板端 ttyS8 以 57600/38400/19200/115200/9600 发 AS608 握手包全部零应答。

**根因(板上实测,2026-09-30)**:RK3576 BSP 中 `sclk_uart2~11` 默认父时钟为
187.5kHz 慢时钟,只有调试台 uart0 被 DTS 显式 assigned 到 24MHz。证据链:
- `dmesg`:`ttyS4/6/8 ... base_baud = 11718` ⇒ uartclk = 187.5kHz,
  物理上限 ≈11.7k 波特,9600 也被分频取整成 11718 的垃圾速率;
- `clk_summary`:`sclk_uart4/6 rate=187500`;`sclk_uart8 rate=24000000` 但
  enable=0(驱动实际拿的是 187.5k 那棵);
- stty 设 57600 内核照单全收但不校验上限,实际线速仍是 187.5k 分频值。

**改法**(抄 uart0 节点的现成写法),`serial@2adb0000` 节点加:

```dts
&uart8 {
    status = "okay";              /* 已是 okay,勿动 */
    assigned-clocks = <&cru SCLK_UART8>;
    assigned-clock-rates = <24000000>;
};
```

**建议顺手**:uart4(`serial@2ad70000`)、uart6(`serial@2ad90000`)同样加上
(备用口,同病)。**验证**:刷后 `dmesg | grep ttyS8` 应显示
`base_baud = 1500000`(24MHz/16)。

### 2.2 SPI 控制器启用 + iccard 子节点(IC 卡)

K7 官方 DTS 的 SPI 控制器默认 disabled(ICCARD_PROTOCOL §10 已注明)。需要:

1. 按实际接线启用对应 SPI 控制器节点(引脚组与 IC 模组 SDA=CS/SCK/MOSI/MISO 对齐);
2. 其下加 iccard 子节点:引用 **RQ(GPIO IRQ)** 与 **RST(GPIO)**、
   `spi-max-frequency`(RC522 类 ≤10MHz,按模组手册取保守值)、reg=片选;
3. IRQ 触发极性按芯片手册(上升沿/高电平),驱动以 DTS flags 为准。

> RQ 中断路线已定案(ICCARD_PROTOCOL §10):IRQ → wait_queue → 驱动内寻卡读
> UID → 唤醒 poller。不用 DMA,环形缓冲 ≥8 帧,驱动不业务去重。

### 2.3 明确不动的

- uart4/6/8 的 `status`/pinctrl:已 okay,已核无引脚冲突;
- WAK 用的 GPIO2_D6:软件双未占用,sysfs 直接用,**不进 DTS**;
- rootfs:termios/sysfs/EPOLLPRI 均现成可用,零改动。

## 3. IC 卡驱动需求(.ko,用户实现)

- **完整契约**:`ICCARD_PROTOCOL.md` §2~§6——节点 `/dev/dg_iccard0`、24B 帧
  (magic 'ICC0' + uid)、`poll(POLLIN)`/阻塞 `read`(close 唤醒)、
  `ioctl DG_ICCARD_IOC_FLUSH/STATS`、错误语义(-EAGAIN/-EIO)。
- **部署形态**:开发期 `.ko` 在 VM SDK 内核树内编(vermagic 与板上 6.1.75 严格
  一致,WSL 编的装不上);`make M=` 增量迭代;定稿转 built-in 改 `.config` 重编。
- **开机自动加载**:buildroot overlay init 脚本 insmod,**序号在 S60 之前**。
- **失败隔离**:ko 不在线 → 应用走"读卡器未就绪"降级,整机不受影响。

## 4. 指纹模块(无内核驱动,只需 §2.1)

- 纯 UART 从设备,应用层 termios 直收发,**不写任何内核代码**;
- 接线定值:uart8 / 57600 8N1 无流控;WAK=GPIO2_D6(94);VTI 必须 3.3V;
- 协议细节、指令序列、确认码语义:`FINGERPRINT_PROTOCOL.md`(v1 已冻结,
  其中【标准推导】条目待刷机后真机逐条验证,见该文 §6 清单)。

## 5. 编译与烧录

1. VM SDK:改板级 dts(§2.1 必须 + §2.2 随 IC 驱动)→ `./build.sh kernel`
   (增量分钟级;IC 驱动 ko 同树内 `make M=` 出 .ko);
2. 烧录:**只刷 boot 分区**(改 DTS 不用全量;RKDevTool"下载镜像"按
   FLASHING.md 地址表,Loader/parameter 两行填 0x0);update.img 一键烧也可;
3. ko 迭代不刷机:推板 `insmod`/`rmmod` 秒级换;
4. 应用侧经 git 流转,勿跨机复制目录。

## 6. 刷机后验收清单(板上依次执行,一次跑完)

```bash
# ① 时钟修复确认:应显示 base_baud = 1500000
dmesg | grep ttyS8

# ② AS608 握手(0x13 验证口令):期望 13B 应答
#    EF 01 FF FF FF FF 07 00 03 00 00 00 0A
stty -F /dev/ttyS8 57600 raw -echo clocal -crtscts
printf '\xEF\x01\xFF\xFF\xFF\xFF\x01\x00\x07\x13\x00\x00\x00\x00\x00\x1B' > /dev/ttyS8 &
timeout 1 cat /dev/ttyS8 | od -An -tx1
# (更稳:用 ssh 单连接发收,或等 fp_provider 上板后看日志)

# ③ 读模板总数(0x1D):期望 07 0005 0000 <N 2B> <sum>
# ④ WAK 复测(GPIO2_D6):按压时 value 应保持翻转直到松手(此前节拍器假象待复核)
echo 94 > /sys/class/gpio/export; echo in > /sys/class/gpio/gpio94/direction
echo both > /sys/class/gpio/gpio94/edge; watch -n0.2 cat /sys/class/gpio/gpio94/value

# ⑤ IC 卡:insmod dg_iccard.ko 后节点出现,贴卡看帧
ls -l /dev/dg_iccard0; timeout 3 cat /dev/dg_iccard0 | od -An -tx1
```

②~④ 通过后回报 `FINGERPRINT_PROTOCOL.md §6` 验证清单逐项勾销;⑤ 数据帧样例
用于关闭 ICCARD_PROTOCOL §11 开放项。
