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
| `dg-ota-publish` | MQTT OTA 生产发版一条龙(上传分发服务器 + retained 公告) | `dg-ota-publish [--version x.y.z] [--notes S]`(见下「生产发版」) |
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
- 设备自报版本(`DG_FW_VERSION`)在 **configure 期**注入,`dg-build` 增量构建
  **不刷新**——要让板上 /api/device、关于设备页与 OTA 版本比较对上当前提交,
  构建前用 `dg-build -c`
- 推板走板上 OTA A/B:装非活动槽 + symlink 原子切换,服务零停机;新包连续
  3 次秒退(<15s)自动回滚旧槽,不会变砖
- **md5 与上一轮相同 = 推了旧包**(链接失败滞留旧产物),先查 dg-build 输出

### 推板分档(dg-deploy)

| 形式 | 行为 |
|---|---|
| `dg-deploy [IP]` | 默认 `-all`:语言包 + 自启脚本 + 程序(旧行为;**S60 脚本变更后/首次必须用**) |
| `dg-deploy -app [IP]` | 只走 OTA 槽位链路升程序,语言包/自启脚本不动(日常改代码用) |
| `dg-deploy -res [IP]` | 只推语言包 + 自启脚本,不切槽不重启(改翻译用;重启进程生效) |
| `-r` | 推完 tail -f 板上日志(tail -f 语义**不自动退出**;脚本化用 `timeout 240 dg-deploy <IP>` 或去掉 -r) |

验证三件事:输出 `已切槽 X → Y(版本 xxx)` 且版本 = 刚推的 commit;`md5 一致`;
日志无 ERROR。**板上 eth0 已是 DHCP(2026-10-14 起直连路由器)**,地址可能随租约
漂移;连不上先 Windows 侧 `ping doorguard.local`(mDNS)拿新地址,再同步 DOORGUARD_IP。

### 生产发版:MQTT 自动升级(dg-ota-publish,2026-10-15 全链落地)

与 `dg-deploy` 的差别:deploy 走 **ssh 直推**(开发机在场);publish 走
**broker 公告**——板端(`network.ota_auto_update=1`)收到新版公告后**自动
下载→sha256 校验→装非活动槽→重启**,开发机只需能连 homeserver。发版四步:

```bash
git tag v1.0.2 && git push origin v1.0.2   # ① 点分 tag(铁律,见下)
source env/env.sh && dg-build -c           # ② -c 让 DG_FW_VERSION = 新 tag
dg-test                                    # ③ 推板纪律:全绿才发
dg-ota-publish --notes "本次改动一句话"      # ④ 上传+retained 公告,一条龙
```

- **版本必须点分数字**(v1.0.2 格式):设备端版本比较只认点分前缀,git
  describe 哈希永远不会被判「新」;同点分值重发也不触发升级(第 3 段后的
  增量后缀不参与比较)。脚本对非点分版本直接拒发(`--force` 强发设备端同样忽略)
- 缺省产物取 `$DG_BUILD_DIR/door-guard`,版本取 `git describe --tags`;
  二进制作为**裸 ELF** 上传到服务器 `/opt/doorguard-fw/door-guard.bin` 并公告
  其 sha256/size(设备端按字节流校验,不吃 .ota 容器)
- 端点与凭据:homeserver(192.168.2.150)上容器 `mosquitto-doorguard`
  (1883)+ `nginx-doorguard`(8088);broker 口令只在服务器
  `/opt/mosquitto/config/dg.pass`(600),公告经 ssh 在服务器侧发布——**口令
  不落开发机**。要换服务器/端口/主题见下「环境变量」的 `DG_FW_*`/`DG_MQTT_*`
- 发布后验证:`mosquitto_sub -h 192.168.2.150 -t doorguard/ota/state -v`
  看进度(downloading→staged),板日志 `[OTA-UPD]` 看全链;部署拓扑与回滚
  演练口径 = DEV_HANDBOOK §7.4(**回滚演练发坏包后必须清 retain**:
  `mosquitto_pub -r -n -t doorguard/ota/version`,否则 FAILED 后重启的进程
  会重收公告反复下载)

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

- tsan 在高熵 ASLR 内核上**起不来**(`FATAL: ThreadSanitizer: unexpected
  memory mapping`,2026-10-15 实测):进 tsan 构建目录
  `cd $DG_TEST_BUILD_DIR-tsan` 用 `setarch $(uname -m) -R ./tests/test_xxx`
  单跑(只关本进程 ASLR,不动系统配置)

### 新机器初始化

装好 cmake / make / node / python3 后:

```bash
source env/env.sh && dg-tc-install && dg-build
```

- 裸板/重刷 rootfs 后**首次安装不走 dg-deploy**(它只做已装应用的 A/B 升级):
  先 `bash board/restore-app.sh`(board/README.md),之后日常才用 dg-deploy

## 环境变量

env.sh 自动作业的(一般不用动):

| 变量 | 说明 |
|---|---|
| `DOORGUARD_ROOT` | 仓库根(source 时自动定位) |
| `DOORGUARD_IP` | 板 IP,默认 192.168.2.96(DHCP 租约漂移时用 doorguard.local 重取) |
| `DG_BUILD_DIR` / `DG_TEST_BUILD_DIR` / `DG_PC_BUILD_DIR` | 构建/测试/模拟器目录。WSL 内默认 `~/dg-build/<类>-<仓库路径哈希>`(ext4——/mnt/c 的 drvfs 慢 I/O 是编译耗时大头);**改回盘内:显式 export 覆盖即可** |

可选覆盖(可写进 `~/.bashrc`):

| 变量 | 说明 |
|---|---|
| `DOORGUARD_TTY` | 串口设备(默认 /dev/ttyUSB0) |
| `DG_TC_ROOT` | 工具链根目录(默认 ~/dg-toolchain) |
| `DG_BUILD_TYPE` | dg-build 构建类型(默认 Release=-O3;要 Debug 显式指定) |
| `DG_FONT_TTF` / `DG_FONT_LATIN` | 字库源字体(默认系统 DroidSansFallbackFull / DejaVuSans) |
| `DOORGUARD_SSH_PORT` / `DOORGUARD_BIN` | dg-deploy 备用路(Windows 侧推板):ssh 端口转发 / 产物显式路径(.exe 绕执行位检查,见 DEV_HANDBOOK §7.1) |
| `DG_FW_HOST` / `DG_FW_DIR` / `DG_FW_PORT` | dg-ota-publish 分发服务器(默认 root@192.168.2.150 / /opt/doorguard-fw / 8088) |
| `DG_MQTT_BROKER` / `DG_MQTT_TOPIC` / `DG_MQTT_USER` / `DG_MQTT_PASSFILE` | dg-ota-publish broker 参数(默认容器 mosquitto-doorguard / 前缀 doorguard / 账号 doorguard / 服务器侧 /opt/mosquitto/config/dg.pass) |

## 分工边界

- 本目录脚本 = **WSL 侧** door-guard 应用开发(dg-* 全家桶)
- **VM 侧**固件/内核/rootfs 全量编译 = SDK 的 `./build.sh`(DEV_HANDBOOK §6)
- 工具链构成与版本配套关系 = `docs/tech/TOOLCHAIN.md`
- 板端走查/抓屏/触摸注入等真机工具 = `tools/board-walk/`(DEV_HANDBOOK §4)
