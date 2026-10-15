# MQTT 固件自动升级——技术文档与部署成果

> 2026-10-15 全链落地并验收。设备 ↔ 平台的自动升级通道:**控制面走 MQTT broker
> (公告/状态/命令),数据面走 HTTP 定长直链(升级包本体)**;板端复用既有
> A/B 槽位与秒退回滚安全网,服务器端零自研常驻代码(两个标准容器 + 一个发布脚本)。
> 部署速查与运维口径 = DEV_HANDBOOK §7.4;本文是完整技术细节与成果存档。

## 0. 成果速览

| 项 | 结论 |
|---|---|
| 真实升级 | v1.0.1(tag 打在 ca10b03):公告 → 自动下载(进度 451‰→999‰)→ sha256 校验闭环 → 装非活动槽 → 重启运行 v1.0.1;活动槽 sha256 与产物逐字节一致;重收 retained 公告正确忽略(无循环) |
| 回滚演练 | 坏包 1.0.2(随机 1KB):公告 1s 被消费后清 retain → 装 B 槽 → **7 秒后「连续 3 次秒退,回滚 → dg_app.A」**,应用稳定,公告零重复消费 |
| 宿主测试 | 52/52 全绿(test_mqtt / test_ota_update / test_stream 等);tsan 因 WSL 高熵 ASLR 需 `setarch -R` 跑,48/52(3 处 race 全在 test_stream 假相机桩,生产侧无 race 报告) |
| 板上验收 | cmd ping/status 往返正常、cmd/open 默认拒绝;WS 视频探针 PASS(10.3fps、SOI 合法);/api/device 自报 v1.0.1;v1.0.1 启动以来 0 ERROR;4 模型就绪、AS608 正常、指纹模组/板端 2:2 一致 |
| 入库 | b225039(部署+工具)/ 33a150a(验收)/ dac9cbb(env README 教程);tag v1.0.1 |

## 1. 架构与设计取舍

```
开发机(WSL)                  homeserver 192.168.2.150                  板 192.168.2.96
─────────────                ────────────────────────                  ─────────────
dg-ota-publish
  │ scp 裸二进制 ──────────→ nginx-doorguard:8088
  │                           /opt/doorguard-fw/door-guard.bin
  │ ssh 服务器侧读口令
  │ mosquitto_pub -r ──────→ mosquitto-doorguard:1883
  │   retain 公告 JSON         (broker 持有最后一条公告)
  │                           │ retained 下发
  │                           ├─ status/hello/event/* ←──────────── mqtt_service(传输层)
  │                           ├─ cmd/+ ──────────────────────────→ 命令分发 → rsp/<name>
  │                           └─ ota/version 公告 ────────────────→ ota_update(业务层)
  │                           ←─ ota/query 手动检查 ──────────────┤    ├─ 解析+版本比较
  │                           ←─ ota/state 进度/终态 ─────────────┘    └─ ota_auto_update=1 → 下载线程
  │                                                                        ↓ HTTP GET 8088(定长直链)
  │                                                                   ota_service 流水线(流式 sha256)
  │                                                                   → /var/lib/door-guard/ota_staged.bin
  │                                                                   S60 ota_watch → 装槽/切换/秒退回滚
```

三个核心取舍:

1. **控制面/数据面分离**:升级包是 MB 级载荷,不过 broker(MQTT 3.1.1 无合适的大载荷
   语义);MQTT 只传几百字节的公告,包本体走 nginx 静态直链。
2. **定长直链而非 https/chunked**:设备端 `ota_http` 按「够用且显式失败」实现——
   只认 http、必须 Content-Length、chunked 显式拒绝、≤3 次重定向。静态文件服务器
   直出即可满足,不为固件场景引入流式解码与 TLS。
3. **retain 是方案的支点**:broker 持有最后一条公告,设备每次上线/重连都会收到,
   平台不需要常驻在线;设备端「不比当前新就忽略」保证重放无副作用。唯一需人工
   干预的场合是回滚演练——坏公告被消费后必须清 retain(见 §5.3)。

## 2. 板端实现

### 2.1 传输层 `door-guard/services/mqtt/mqtt_service.c`(655 行)

只做通道不做业务。线程契约:**mongoose `mg_*` 只在 netcore loop 线程**
(`modules/net/netcore.c` 持有唯一 `mg_mgr`,web/mDNS/NTP/MQTT 同一事件循环)。

- **发**:业务线程 → `mqtt_publish_json()` → 深度 8 环形邮箱(满丢最旧并计数,
  遥测宁丢不堵)→ `netcore_post(drain_mailbox)` 在 loop 线程真正发出。
- **收**:平台命令落 `<prefix>/cmd/+` → `cmd_dispatch`(c:206)查注册表再落内置;
  处理函数在 loop 线程执行、必须非阻塞;返回值自动变 `{"ok":..}` 发 `rsp/<name>`。
- **跨线程状态**:仅三个原子(running/enabled/connected)+ 邮箱;注册表
  `s_cmds[]`/`s_subs[]` mutex 保护、变更经 `netcore_post` 应用。

| 主题 | 方向 | QoS | retain | 说明 |
|---|---|---|---|---|
| `<p>/status` | 设→平 | 0 | 是 | 上线 `{"state":"online"}`;LWT 自动落 offline;停服发告别再关 |
| `<p>/hello` | 设→平 | 0 | 否 | `{"ip":"..."}` |
| `<p>/event/auth` `event/service` `event/alarm` | 设→平 | 0 | 否 | 业务上报,未连接不发 |
| `<p>/cmd/+` | 平→设 | 1 | — | 订阅;内置 ping/status/open + `mqtt_cmd_register` 扩展(≤8 个) |
| `<p>/rsp/<name>` | 设→平 | 0 | 否 | 命令应答 |
| `<p>/ota/version` | 平→设 | 1 | 平台侧 | retained 公告,`mqtt_sub_register` 订阅,重连补订 |
| `<p>/ota/query` | 设→平 | 0 | 否 | 手动「检查更新」,平台收到后重发公告 |
| `<p>/ota/state` | 设→平 | 0 | 否 | 下载进度/终态(downloading permille → staged/failed) |

可靠性:断线退避重连 5→60s 指数(CONNACK 成功复位);keepalive 30s;3×keepalive
无入包判半开强制重连。

`cmd/open` 的安全语义:默认拒绝(`mqtt.allow_remote_open=0` → `{"err":"not-allowed"}`);
即便授权也不直接动继电器——发 `EV_MQTT_CMD` 事件上总线,开门必须走 access 流程
留痕(**消费端目前是预留契约,尚未接业务**)。

### 2.2 业务层 `door-guard/services/ota/ota_update.c`

装配顺序 `main.c`:`config → netcore → web → mdns → mqtt → ota_update → ui`;
启动即 `mqtt_sub_register("ota/version", ...)`(mqtt 未启用则整体空转)。

公告处理流水线(`ota_update_on_announce`,c:188):

1. **解析校验**:JSON 必填 version / sha256(64hex) / size(>0);date/notes/url 可选。
2. **版本比较** `ota_version_newer`(c:156):取点分数字前缀逐段比,`v` 前缀与
   第 3 段后缀(`-N-gxxx`)忽略。边界刻意为之:当前版本解析不了(哈希串)→
   任何点分候选都算新;候选解析不了 → 不当新版。
3. **去抖**:下载进行中来新公告忽略(不叠会话);FAILED 后同版本重公告给一次
   重试机会(retained 重连必到,重试频率天然受公告节奏约束)。
4. **分发**:为新 → AVAILABLE;`cfg network.ota_auto_update=1` 立即
   `ota_update_apply()`,否则只发事件让关于设备页亮「立即更新」。

下载线程(c:274):url 取公告值(空回落 `cfg network.ota_url`),`ota_http`
打开流后 **Content-Length 与公告 size 对拍**,字节流喂 `ota_service` 流水线:
边收边算 sha256 → `ota_finish` 校验闭环 → 原子 rename 为
`/var/lib/door-guard/ota_staged.bin`(+`.sha256`/`.ver` sidecar)。
进度走 `EV_NET_OTA_UPDATE`(UI)+ `ota/state`(平台);所有失败路径统一收敛
FAILED,**坏包不落 staged**。**交接边界到此为止**:装槽/切换/回滚不在 C 代码里。

### 2.3 板端兜底 `board/rootfs-overlay/etc/init.d/S60doorguard`

`ota_watch` 每 2s 查暂存文件(2s 大小稳定=上传完整)→ `sha256sum -c` 二次复核 →
拷进**非活动槽** → symlink 原子切换(`/root/door-guard`)→ `killall door-guard`
由监督循环拉起新版。监督循环统计运行时长,**<15s 秒退连续 3 次自动切回旧槽**
(回滚演练实测 7 秒弹回)。槽位:`/root/dg_app.A` / `.B`。

### 2.4 版本注入与配置键

- `DG_FW_VERSION` 由 CMake **configure 期** `git describe --tags` 注入
  (`door-guard/CMakeLists.txt:418-432`)——`dg-build` 增量构建不刷新,
  发版前必须 `dg-build -c`;运行时经 `ota_update_current_version()`、
  `/api/device`、关于设备页暴露。
- 配置(`configs/default.json` 模板,现用值在 `/userdata/doorguard/cur_config.json`):

| 键 | 默认 | 说明 |
|---|---|---|
| `mqtt.enabled` / `uri` | false / 空 | 通道开关与 broker 地址(`mqtt://192.168.2.150:1883`) |
| `mqtt.topic_prefix` / `client_id` | doorguard / 空(自动) | 主题前缀 / `doorguard-<IP末段>` |
| `mqtt.username` / `password` | 空 | broker 认证 |
| `mqtt.allow_remote_open` | false | cmd/open 授权,安全默认拒 |
| `network.ota_auto_update` | **0** | 检到新版自动下载升级(部署侧显式开,关于设备页可切) |
| `network.ota_url` | 空 | 公告缺 url 时的下载回落地址 |

## 3. 服务器端实现(homeserver 192.168.2.150)

平台侧刻意做成「哑」的——无自研常驻代码:

### 3.1 `mosquitto-doorguard`(eclipse-mosquitto:2,端口 1883)

```ini
# /opt/mosquitto/config/mosquitto.conf
listener 1883 0.0.0.0
allow_anonymous false
password_file /mosquitto/config/passwd     # 账号 doorguard;口令在 /opt/mosquitto/config/dg.pass(600 root)
persistence true                            # retain 落盘,broker 重启不丢「最新版本」公告
persistence_location /mosquitto/data/
log_dest file /mosquitto/log/mosquitto.log
```

`--restart unless-stopped`;data/log 卷属主 1883:1883。
**唯一运维坑:passwd 文件属主必须 1883:1883**——root:600 会让 mosquitto
起来 3 秒后 exit 13 循环崩溃(2026-10-15 实测)。

### 3.2 `nginx-doorguard`(nginx:alpine,8088→80)

只读挂载 `/opt/doorguard-fw`,静态直出——天然满足 Content-Length 定长要求
(实测响应带 `Content-Length` + `Accept-Ranges: bytes`)。

### 3.3 发布侧 `env/bin/dg-ota-publish`

```bash
dg-ota-publish [二进制路径] [--version x.y.z] [--notes S] [--force]
```

- 产物缺省 `$DG_BUILD_DIR/door-guard`,版本缺省 `git describe --tags`;
  **非点分版本直接拒发**(`--force` 强发设备端同样忽略);notes 去引号防 JSON 注入。
- 流程:scp 裸二进制 → `ssh root@192.168.2.150` 在**服务器侧**读 dg.pass 发
  `mosquitto_pub -r` retained 公告——口令全程不落开发机。
- 覆盖项:`DG_FW_HOST/DG_FW_DIR/DG_FW_PORT`、`DG_MQTT_BROKER/TOPIC/USER/PASSFILE`
  (缺省 root@192.168.2.150 / /opt/doorguard-fw / 8088 / mosquitto-doorguard /
  doorguard / doorguard / /opt/mosquitto/config/dg.pass)。

服务器 ssh:root **密码登录不可用**(口令属 `olwhistle` 账号);WSL 公钥经
olwhistle+sudo 写入 `/root/.ssh/authorized_keys` 免密。

公告格式(发布侧 → 设备端解析):

```json
{"version":"1.0.1","date":"2026-10-15","url":"http://192.168.2.150:8088/door-guard.bin",
 "sha256":"<64hex>","size":8166808,"notes":"..."}
```

## 4. 硬契约汇总(违反即静默不升级)

| # | 契约 | 原因 |
|---|---|---|
| 1 | 公告 url 指向**裸 ELF 二进制**,不是 .ota 容器;sha256/size 是裸二进制的 | 设备端把 HTTP 字节流直接喂校验流水线 |
| 2 | HTTP 响应必须定长(Content-Length 必现);https/chunked 被拒;≤3 次重定向 | ota_http 显式失败设计 |
| 3 | 版本必须**点分数字**(v1.0.2);同点分值重发不触发升级 | 版本比较只认点分前缀,哈希串永不判「新」 |
| 4 | 换 tag 后构建必须 `dg-build -c` | DG_FW_VERSION 是 configure 期注入 |

## 5. 运维手册

### 5.1 日常发版(四步)

```bash
git tag v1.0.2 && git push origin v1.0.2   # ① 点分 tag
source env/env.sh && dg-build -c           # ② 版本注入
dg-test                                    # ③ 全绿才发
dg-ota-publish --notes "本次改动一句话"      # ④ 上传+公告,板端约 1 分钟内自动升级
```

验证:`mosquitto_sub -h 192.168.2.150 -u doorguard -P <口令> -t doorguard/ota/state -v`
(downloading→staged);板上 `tail -f /var/log/door-guard.log` 看 `[OTA-UPD]` 全链;
完成后 `readlink /root/door-guard` 换槽、`/api/device` 版本更新。

### 5.2 手动公告(不经脚本)

```bash
ssh root@192.168.2.150 "docker exec mosquitto-doorguard mosquitto_pub \
  -h 127.0.0.1 -u doorguard -P \$(cat /opt/mosquitto/config/dg.pass) \
  -r -t doorguard/ota/version -m '{\"version\":\"1.0.2\",...,\"sha256\":\"...\",\"size\":N,\"url\":\"http://192.168.2.150:8088/door-guard.bin\"}'"
```

### 5.3 回滚演练(测安全网)——必须清 retain

发坏包(sha 与公告匹配的随机文件)→ **公告被板端消费后立即清 retain**:
`mosquitto_pub ... -r -n -t doorguard/ota/version`。否则 FAILED 后重启的进程
重收 retained 坏公告会反复下载成循环(演练实测:1s 消费 → 清 retain → 7s 回滚,
公告零重复消费)。

### 5.4 故障排查速查

| 症状 | 先查 |
|---|---|
| 板不上线(无 status online) | 服务器 `docker ps` 看 mosquitto 状态;板上日志 grep -a mqtt;板网连通(ping doorguard.local) |
| mosquitto 容器循环重启 | 日志 `Unable to open pwfile` → passwd 属主改 1883:1883 |
| 公告收到但「不比当前新」 | 版本没打点分 tag / `dg-build -c` 没跑(板上还是旧版本串) |
| 下载失败 err≠0 | 公告 size/sha256 与文件不符;直链缺 Content-Length;服务器 nginx 挂了 |
| 下载 staged 但没切槽 | S60 的 ota_watch 活着没(`cat /var/run/door-guard-ota.pid`) |

## 6. 边界与后续

- **升级对象 = 应用二进制**(door-guard ELF,复用 A/B 槽)。整机 rootfs/uboot 级
  A/B(app_a/app_b 分区 + uboot env 切槽)是 OTA_PLAN 明确待办(B10),不在本通道。
- 旁路服务(iccard_daemon 等)**不在升级包内**,重刷 rootfs 仍走 `board/restore-app.sh`。
- `EV_MQTT_CMD`(远程开门事件)消费端未接;接入时必须走 access 流程留痕。
- tsan 遗留:test_stream 假相机桩(`s_claim_on`/`s_frame` 无同步)3 处 race,
  慢机下会漏帧挂 `n>=2` 断言——待把桩改 atomics;生产侧 web_stream 无 race 报告。
- 全量重刷(boot/rootfs)仍走 RKDevTool / upgrade_tool(docs/tech/FLASHING.md),
  与本通道互不影响。

## 7. 相关文档

- 部署速查/网络口径:`docs/DEV_HANDBOOK.md` §7.4 / §7.2
- OTA 总体规划(A/B 槽位、分区级待办):`docs/tech/OTA_PLAN.md`
- 两套件的模块文档:`door-guard/services/mqtt/README.md`、`door-guard/services/ota/README.md`
- 发版教程与示例:`env/README.md`「生产发版」节
