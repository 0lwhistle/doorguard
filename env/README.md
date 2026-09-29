# env/ — 环境与脚本

在仓库**任意位置**执行一次:

```bash
source env/env.sh
```

之后 `env/bin/` 下的脚本直接敲名字可用(不用带路径):

| 脚本 | 用途 | 常用形式 |
|---|---|---|
| `dg-build` | 交叉编译 door-guard | `dg-build`(增量)/ `dg-build -c`(全新配置) |
| `dg-deploy` | 推板(OTA A/B 链路) | `dg-deploy <IP>`(默认 -all:资源+程序)/ `dg-deploy -app <IP>`(只升程序)/ `dg-deploy -res <IP>`(只推语言包/自启脚本)/ `-r` 推完跟日志 |
| `dg-tc-install` | 安装/更新交叉工具链到 `~/dg-toolchain`(md5 校验→解压) | `dg-tc-install` |
| `dg-serial` | 串口控制台(1500000 8N1) | `dg-serial` |

可写进 `~/.bashrc` 的变量:

```bash
export DOORGUARD_IP=192.168.x.x    # dg-deploy 省掉 IP 参数
export DOORGUARD_TTY=/dev/ttyACM0  # 串口设备名(默认 /dev/ttyUSB0)
export DG_TC_ROOT=/path/to/toolchain  # 工具链根目录(默认 ~/dg-toolchain)
```

新机器初始化 = 装好 cmake/make 后:`source env/env.sh && dg-tc-install && dg-build`。

## 分工边界

- 本目录脚本服务 **WSL 侧**(door-guard 应用开发)
- **VM 侧**固件/内核/rootfs 编译走 SDK 的 `./build.sh` 体系,见 `docs/DEV_HANDBOOK.md` §6
- 工具链构成与版本配套关系见 `docs/tech/TOOLCHAIN.md`
