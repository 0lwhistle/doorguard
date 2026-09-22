# 统一网络层重构设计:mongoose 单事件循环(web / OTA / NTP / mDNS)

- 日期:2026-09-22
- 状态:已批准(用户口头确认;附加需求:web 管理页同一时刻只允许一个管理员登录)
- 决议:mongoose 7.23 以 **GPLv2 路径**使用(项目本身开源;若将来闭源商用需购 Cesanta 商业授权或回退 MIT 库)
- 关联:`spec-network.md`、`door-guard/services/web/README.md`、`docs/architecture-v2-proposal.md` §3.3(线程归属表,落地时同步修订)

## 1. 目标与非目标

**目标**
1. 新增模块层组件 `modules/net/netcore`(mongoose 胶水,零业务):独占一个事件循环线程,
   承载全部网络 I/O——HTTP/WS(web 上位机 + OTA 上传端点)、UDP(mDNS)、SNTP(NTP)。
2. 业务服务( web/ota/ntp/mdns )保留各自协议与业务逻辑,传输部分迁移到 netcore。
3. 净减 4~5 个线程;civetweb(含 OpenSSL shim)退役删除。
4. web 上位机**单会话策略**:同一时刻仅一个有效会话,新登录成功即吊销其余会话。

**非目标**
- 前端 Vue 工程一字节不改;HTTP API 路由/状态码/JSON 结构不变。
- mDNS/NTP/netlink 之外的模块不动;`modules/net` 的 net_info(netlink/IP 查询)保持现状。
- 不做 TLS、不做高帧率视频推流(仍为占位)。

**硬不变量(验收线)**
- 现有测试全部原样通过:`web_test.sh`(63 项)、`ws_test.py`、`mdns_test.sh`、
  `test_mdns_wire`、`test_ota`、`test_web_auth`(按单会话策略重写后)、前端 vitest 44 项、
  `frontend_check.py`。
- `web_server.h` 对外 API(start/stop/heartbeat_ms)签名不变;`EV_NET_*` 事件契约不变。

## 2. 架构

```
services/web    路由/鉴权/会话/静态页/WS 业务(逻辑保留)→ 注册到 netcore
services/ota    暂存 + sha256(逻辑不动;仅可能新增非阻塞余量查询)
services/ntp    策略(开机一次/手动触发/结果事件)保留 → SNTP 客户端走 loop
services/mdns   协议状态机 + mdns_wire 编解码保留 → UDP 监听走 loop
modules/net     net_info(现有)+ netcore(新)
        ↓
modules/net/netcore   事件循环线程 + 注册接口 + 跨线程投递 + 心跳
        ↓
third_party/mongoose 7.23(vendored;GPLv2)
```

层次合法性:services → modules 是五层栈合法依赖方向;mongoose 定位为项目的网络框架
(如同 LVGL 之于 UI),netcore.h 直接包含 mongoose.h。**线程契约:仅 loop 线程回调内可
调用 mongoose 连接 API;其他线程一律 `netcore_post()` 投递。**此契约写入模块 README。

### 2.1 netcore 接口(netcore.h)

```c
int     netcore_start(void);      /* 线程 + mg_mgr_init + mg_wakeup_init */
void    netcore_stop(void);       /* 停轮、关 mgr、join;幂等 */
bool    netcore_running(void);
int64_t netcore_heartbeat_ms(void); /* loop 每轮刷新;看门狗语义不变 */
void    netcore_post(void (*fn)(void *arg), void *arg); /* 任意线程 → loop 执行;
                                                           mg_wakeup 唤醒;停服后静默丢弃 */
struct mg_mgr *netcore_mgr(void);   /* 注册用:仅限 start 后、stop 前的装配期调用 */
```

- HTTP/WS/UDP/SNTP 的注册直接用 mongoose 原语(`mg_http_listen` / `mg_ws_upgrade` /
  `mg_open_listener` / `mg_sntp_connect` / `mg_timer_add`),不做抽象层包装(YAGNI)。
- `netcore_post` = 互斥队列 + socketpair waker(`mg_wakeup_init`/`mg_wakeup`);
  100ms `mg_timer_add` 兜底轮,防唤醒竞态。
- 心跳:loop 线程每轮 poll 刷新;`web_server_heartbeat_ms()` 变为透传。

### 2.2 线程账

| 现状 | 目标 |
|---|---|
| civetweb 4 worker + WS 推送线程 | netcore 1 loop 线程 |
| mdns 自有 pthread + raw socket | 并入 loop(回调 + timer) |
| ntp 每次触发 spawn 线程跑 system(ping+chronyc) | SNTP 异步事件,零 spawn;联网探测改 net_info_is_online() |
| netlink 监听(net_info) | 不动 |

WS 连接表退化为 loop 线程私有结构(无锁);消息队列保留(总线线程生产,溢出丢最旧并计数)。

## 3. 各业务迁移要点

### 3.1 web(HTTP + WS + 静态页 + OTA 端点)

- `MG_EV_HTTP_MSG` 表驱动分派:method+uri 精确匹配;405(改状态接口仅 POST)/
  `/api/` JSON 404 / 非资源路径 SPA 回退,语义与现实现逐一对应。
- 响应工具改 mongoose 输出;保留 `X-Content-Type-Options: nosniff`、`Cache-Control: no-store`。
- WS:`MG_EV_HTTP_MSG` 阶段验 `?token=`,失败回**完整 401** 再关(不升级);成功
  `mg_ws_upgrade`。`MG_EV_WS_MSG` 忽略(保持连接)。
- WS 推送:总线回调 → 入队(现有环形队列)→ `netcore_post` 排空 → `mg_ws_send` 逐连接。
- OTA 上传:`MG_EV_HTTP_HDRS`(头先至)校验 token + manifest 头 → `ota_begin`;
  `MG_EV_READ` 增量消费 `c->recv` 喂 `ota_write_chunk`,不整体缓冲 64MB。
- PBKDF2(登录/改密,~几十 ms)与 SQLite 查询(ms 级)在 loop 内执行:局域网 1–2 客户端
  可接受;板上压测确认。

### 3.2 OTA 背压(风险最高,首日 spike 决策门)

- 问题:`ota_write_chunk` 满时阻塞在 `cv_space`(带超时上界,但秒级阻塞在 loop 不可接受);
  且若 mongoose 在 `c->recv` 不消费时**继续读 socket**,iobuf 会无界增长。
- spike 验证:不消费 `c->recv` 时 mongoose 是否暂停读。是 → 纯流式;否 → 给 ota_service
  增加 `ota_can_accept()`(环形缓冲余量查询),loop 只喂余量以内字节,其余留 recv。
  两个落点都保留 OTA 上传期间其他连接可用的验收项(web_test.sh 已有 OTA 闭环用例,
  补"上传中并发打 /api/device 不阻塞"断言)。

### 3.3 NTP:chrony → 应用内 SNTP

- `ntp_service` API 与事件不变(`ntp_service_trigger()`、`EV_NET_NTP_TRIGGER`、
  `EV_NET_NTP_RESULT`);实现换 `mg_sntp_connect`(loop 内异步,超时 ~10s),
  成功 `settimeofday` 步进;开机自动一次 / 菜单按钮 / 上位机 202+WS 三条路径不变。
- 联网探测:`system("ping")` 外调删除,改 `net_info_is_online()`。
- **rootfs 侧需停用 chrony 常驻**(否则与 SNTP 双写系统时间);固件 Phase 落地,
  应用侧先行为准。时间回拨对 access_logs 展示的影响记录到 spec-network。
- 测试:PC 侧写本地 SNTP stub(python,tests/web/sntp_stub.py),回显可控,
  覆盖成功/失败/超时。

### 3.4 mDNS

- `mdns_wire.c` 编解码与探测/通告/应答/goodbye 状态机保留;传输换 netcore UDP 监听
  (224.0.0.251:5353,组播加入走 `c->fd` setsockopt;IP_MULTICAST_LOOP 关)。
- 定时重通告 → `mg_timer_add`;IP 变化重通告/关机 goodbye 由既有事件驱动,不变。
- spike:双网口逐包接口绑定(现实现按来包网口 egress)。若 mongoose 单 socket 无法
  保留该行为,每网口开一个 listener 或保留独立发送 fd;以 mdns_test.sh 实测为准。

### 3.5 web 单会话策略(用户新增需求)

- `web_session_create()` 签发成功后清除其余全部有效会话(新登录必胜,自愈:
  崩溃浏览器不会永久占坑)。同一时刻有效会话数 ≤ 1。
- 滑动续期、`revoke`(logout)、改凭据全吊销语义不变;`WEB_SESSION_MAX` 暂保留
  (结构不动,语义上不再可达)。
- 被踢会话的感知路径沿用现状:下一个 API 请求或 WS 重连得 401 → 前端路由守卫回登录页。
- TDD:先改 `test_web_auth.c` t_sessions(多次 create 后仅最新 token 有效、count==1),
  再改实现。

## 4. 实施阶段(每步测试全绿才进下一步)

1. **netcore 骨架**:CMake 接入 mongoose(host 先通,裁剪 TLS/FILE/内置栈);
   loop 线程、`netcore_post`、心跳 + `test_netcore.c`。web 暂仍在 civetweb 上。
2. **单会话策略**:TDD 落地(见 §3.5),`web_test.sh` 补"二次登录吊销首登录 token"用例。
3. **web 上 loop**:§3.1 全部内容 + OTA spike(§3.2);civetweb 退役,删目录与 CMake 段。
4. **mDNS 上 loop**:§3.4(spike 先行)。
5. **NTP 换 SNTP**:§3.3。
6. **清理收尾**:删 `ota_port` 死配置(cfg.h/cfg.c/default.json/test_cfg.c);
   更新 spec-network、web/mDNS/NTP README、architecture.md 模块登记(含线程归属表)、
   mongoose README(标记已接入 + GPLv2 决议)、DEVLOG;commit + push。

板上人工验收(需真机,代码就绪后执行):双网口 mDNS、OTA 断点续传、上传与推送并发、
两浏览器互踢、看门狗心跳。

## 5. 风险登记

| 风险 | 处置 |
|---|---|
| OTA 背压机制未知 | 首日 spike,双落点退路(§3.2) |
| mDNS 组播 egress 细节 | spike,以 mdns_test.sh 为准 |
| PBKDF2/SQLite 阻塞 loop | 接受 + 板上压测;超预期再议 worker |
| SNTP 步进换 chrony slew | 接受并记录;rootfs 停用 chrony 随固件 Phase |
| keep-alive 差异(原强制 Connection: close) | 实施时核对 web_test.sh 断言,必要时保留显式 close |
| 回退 | 分阶段提交,任一阶段可 revert;civetweb 删除放在 web 迁移全绿之后 |
