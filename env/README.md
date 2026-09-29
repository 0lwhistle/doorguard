# env/ — 环境与脚本(使用文档)

在仓库**任意位置**执行一次(每个新 shell 都要):

```bash
source env/env.sh
```

之后 `env/bin/` 下所有 `dg-*` 直接敲名字可用(脚本清单见 env.sh 的就绪输出)。
本目录脚本服务 **WSL 侧**(door-guard 应用开发,交叉工具链在 WSL);VM 侧
固件/内核/rootfs 全量编译走 SDK 的 `./build.sh` 体系(DEV_HANDBOOK §6)。

## 脚本总览

| 脚本 | 用途 | 常用形式 |
|---|---|---|
| `dg-build` | 交叉编译 door-guard(产物在 `~/dg-build/aarch64-<hash>/`) | `dg-build`(增量)/ `dg-build -c`(全新配置)/ 透传 cmake 参数 |
| `dg-test` | 宿主 gcc 跑全部单元用例(ctest) | `dg-test` / `dg-test --tsan`(并发组件 SAN)/ `dg-test -c` |
| `dg-build-pc` | PC 模拟器构建(SDL2/WSLg,720×1280) | `dg-build-pc` / `-r`(构建后直接运行)/ `-c` |
| `dg-deploy` | 推板(板上 OTA A/B 链路) | 见下"推板分档" |
| `dg-font` | 语言包变更后重生成中文位图字库 | `dg-font` |
| `dg-frontend` | 重建 web 上位机前端并内嵌进二进制 | `dg-frontend` / `dg-frontend --install`(先 npm ci) |
| `dg-ota-upload` | 生产 OTA HTTP 通道(web 端口流式 + sha256) | `dg-ota-upload <板IP> <升级包>` |
| `dg-tc-install` | 安装/更新交叉工具链到 `~/dg-toolchain`(md5 校验→解压) | `dg-tc-install` |
| `dg-serial` | 串口控制台(**1500000 8N1**,不是 115200) | `dg-serial` |

## 典型流程

### 日常:改 C 代码 → 推板

```bash
git commit                # 版本串来自 git describe,先提交再推
dg-test                   # 必须全绿(推板纪律:测试不过不推板)
dg-build                  # 零告警才算过
dg-deploy -app -r         # 日常只升程序;推完跟踪日志(Ctrl-C 退出不影响板上)
```

- 构建目录/产物路径由 env.sh 自动设(WSL 内在 ext4,见下表);configure 按需
  跑——CMakeLists 变更自动重生成,ui/widgets 新增源文件自动拾取,lvgl 等
  vendored 目录加文件才需要 `dg-build -c`
- 推板走板上 OTA A/B:装非活动槽 + symlink 原子切换,服务零停机;新包连续
  3 次秒退(<15s)自动回滚旧槽,不会变砖
- **md5 与上一轮相同 = 推了旧包**(链接失败滞留旧产物),先查 dg-build 输出

### 推板分档(dg-deploy)

| 形式 | 行为 |
|---|---|
| `dg-deploy [IP]` | 默认 `-all`:语言包 + 自启脚本 + 程序(旧行为;**S60 脚本变更后/首次必须用**) |
| `dg-deploy -app [IP]` | 只走 OTA 槽位链路升程序,语言包/自启脚本不动(日常改代码用) |
| `dg-deploy -res [IP]` | 只推语言包 + 自启脚本,不切槽不重启(改翻译用;重启进程生效) |
| `-r` | 推完 tail -f 板上日志 |

验证三件事:输出 `已切槽 X → Y(版本 xxx)` 且版本 = 刚推的 commit;`md5 一致`;
日志无 ERROR。**板上网络纯静态(192.168.137.100),永远不要切 DHCP。**

### 改了语言包(ui/lang/*.json)

```bash
dg-font                   # 重生成字库 dg_font_cn_*.c
git commit                # font/*.c 与 lang/*.json 一起提交
dg-build && dg-deploy     # 默认 -all:字形(二进制)+ 译文(JSON)两头都要新
```

- 16/26/30 档已并入 GB2312 全集,普通中文不重跑也多能显示;**必须重跑**:
  ① 40px 标题档只含语言表字集;② GB2312 外的字符;③ 新增字符要出现在
  lang json(键或值)里——gen 只扫 zh-CN/en-US 两个文件,新增语言文件要改
  `ui/font/gen.sh` 的文件列表
- `…`(U+2026)源字体无字形,渲染为空——省略号一律写 `...`

### 改了 web 上位机前端(services/web/frontend/)

```bash
dg-frontend               # npm build → pages/ → web_pages.c(内嵌进二进制)
git commit                # pages/ 与 web_pages.c 一起提交
dg-build && dg-deploy -app
```

- web 前端**编译进 app 二进制**(gen_pages.sh 生成 web_pages.c),板上没有
  独立 web 文件——升级 web = 升级 app,没有独立升级包
- WSL 对 npm registry 无网(历史实测),`--install`(npm ci)需要可用网络/
  镜像;node_modules 已就位的克隆可直接构建

### 测试

```bash
dg-test                   # 宿主 gcc 全部用例
dg-test --tsan            # 并发组件另跑 ThreadSanitizer 构建目录
dg-test -c                # 删构建目录全新配置
```

### 新机器初始化

装好 cmake / make / node / python3 后:

```bash
source env/env.sh && dg-tc-install && dg-build
```

## 环境变量

env.sh 自动作业的(一般不用动):

| 变量 | 说明 |
|---|---|
| `DOORGUARD_ROOT` | 仓库根(source 时自动定位) |
| `DOORGUARD_IP` | 板 IP,默认 192.168.137.100 |
| `DG_BUILD_DIR` / `DG_TEST_BUILD_DIR` / `DG_PC_BUILD_DIR` | 构建/测试/模拟器目录。WSL 内默认 `~/dg-build/<类>-<仓库路径哈希>`(ext4——/mnt/c 的 drvfs 慢 I/O 是编译耗时大头);**改回盘内:显式 export 覆盖即可** |

可选覆盖(可写进 `~/.bashrc`):

| 变量 | 说明 |
|---|---|
| `DOORGUARD_TTY` | 串口设备(默认 /dev/ttyUSB0) |
| `DG_TC_ROOT` | 工具链根目录(默认 ~/dg-toolchain) |
| `DG_BUILD_TYPE` | dg-build 构建类型(默认 Release=-O3;要 Debug 显式指定) |
| `DG_FONT_TTF` / `DG_FONT_LATIN` | 字库源字体(默认系统 DroidSansFallbackFull / DejaVuSans) |
| `DOORGUARD_SSH_PORT` / `DOORGUARD_BIN` | dg-deploy 备用路(Windows 侧推板):ssh 端口转发 / 产物显式路径(.exe 绕执行位检查,见 DEV_HANDBOOK §7.1) |

## 分工边界

- 本目录脚本 = **WSL 侧** door-guard 应用开发(dg-* 全家桶)
- **VM 侧**固件/内核/rootfs 全量编译 = SDK 的 `./build.sh`(DEV_HANDBOOK §6)
- 工具链构成与版本配套关系 = `docs/tech/TOOLCHAIN.md`
- 板端走查/抓屏/触摸注入等真机工具 = `tools/board-walk/`(DEV_HANDBOOK §4)
