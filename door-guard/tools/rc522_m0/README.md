# rc522_m0 — MFRC522 读头 SPI 验线工具(里程碑 M0)

在写内核驱动之前,先用用户态 spidev 把「模块 + 接线 + SPI4 链路」验干净,
避免驱动 bug 与接线 bug 互相污染。原理:读 MFRC522 版本寄存器 `0x37`,
应答 `0x91/0x92` 即整条链路通(地址字节 `(0x37<<1)|0x80` + 1 个 dummy)。

## 接线(40pin,对丝印复核)

| 模块脚 | 40pin 脚号 | GPIO | 说明 |
|---|---|---|---|
| 3.3V | 3V3 | — | **勿接 5V** |
| GND | GND | — | — |
| SDA(CS/NSS) | 16 | GPIO1_D0 | 片选,低有效,DTS cs-gpios 已配 |
| SCK | 8 | GPIO4_B0 | — |
| MOSI | 10 | GPIO4_B1 | — |
| MISO | 12 | GPIO4_B2 | — |
| **RST** | 14 | GPIO0_C3 | **M0 阶段直接跳线到 3V3**(悬空可能复位态,版本号读出 0x00/0xFF);M2 起改由驱动 reset-gpios 控制,拆掉跳线 |
| RQ | 9 | GPIO2_D7 | M0 不用,悬空;M3 起作中断 |

## 编译(WSL)

```bash
SDK=/home/olwhistle/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008
TC=$SDK/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
$TC -O2 -Wall -Wextra -static rc522_m0.c -o rc522_m0
```

`-static` 省去板上 glibc 版本顾虑;WSL 宿主可先 `gcc -O2 -Wall rc522_m0.c -o /tmp/x` 做语法检查(不出板也能查编译错)。

## 部署与运行

```bash
scp rc522_m0 root@192.168.137.100:/root/
ssh root@192.168.137.100 '/root/rc522_m0 -r 5'
```

`-r 5` 连读 5 次应全一致;虚焊/干扰用 `-r 0` 常驻观察,按住模块轻晃看会不会漂。

## 判读

| 输出 | 含义 |
|---|---|
| `0x91` / `0x92` | MFRC522 v1.0/v2.0,链路通 → M0 通过 |
| `0x88` / `0x89` | FM17522 等兼容芯片,链路同样算通(驱动侧对卡操作行为基本一致) |
| `0x00` | MISO 无数据:查供电、SCK/MOSI/MISO 是否按 8/10/12 脚、CS 16 脚 |
| `0xFF` | MISO 恒高:查 MISO 是否接错位、供电、RST 是否已拉高 |
| SPI 传输失败 | 节点/权限/控制器问题,`dmesg | grep spi` 看驱动侧 |

退出码:全通 = 0,任何一次异常 = 1(可脚本化)。

## 通过之后

进入驱动里程碑 M1~M4(HANDOFF §6.2):驱动 .c 按契约
`docs/tech/ICCARD_PROTOCOL.md` §2~§6 由你手写,放
`C:\Users\86151\Desktop\doorguard\kernal_driver\k7_rc522\`,
用 `wsl.exe -e bash -c "~/k7-tools/sync-driver.sh k7_rc522"` 迭代;
DTS 节点与 defconfig 由助手在 v2 镜像布好(`~/k7-tools/build-rc522-v2.sh`)。
注意:M1 起模块的 RST 必须从 3V3 跳线改接 14 脚 GPIO,由驱动控制。
