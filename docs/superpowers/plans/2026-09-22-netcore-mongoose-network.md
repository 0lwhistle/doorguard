# 统一网络层重构(mongoose 单事件循环)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 vendored mongoose 7.23 替换 civetweb,新建 `modules/net` netcore 模块(单事件循环线程)承载 web/OTA/NTP/mDNS 全部网络 I/O,并落地 web 单会话策略(仅一个管理员在线)。

**Architecture:** services(web/ota/ntp/mdns)保留业务逻辑,传输挂到 modules/net 的 netcore;所有连接读写只在 loop 线程,跨线程经 `netcore_post()`;civetweb(含 OpenSSL shim)删除。

**Tech Stack:** C11 + pthread、mongoose 7.23(GPLv2)、cJSON、OpenSSL(evp/sha256/rand)、Vue3 前端(零改动)、Python3 验收脚本。

**Spec:** `docs/superpowers/specs/2026-09-22-netcore-mongoose-network-design.md`

## Global Constraints

- 前端 `services/web/frontend/` 与产物 `pages/`、`web_pages.c` **一字节不改**
- HTTP API 路由/方法/状态码/JSON 结构不变;`web_server.h` 三个函数签名不变;`EV_NET_*` 契约不变
- 每个任务收尾:宿主测试全绿才 commit;全程 dg-build(交叉)零警告
- 现有测试是回归线:`web_test.sh` 63 项、`ws_test.py`、`mdns_test.sh`、`test_mdns_wire`、`test_ota`、`test_web_auth`(按新策略重写)、前端 vitest 44 项、`frontend_check.py`
- 新模块纪律:architecture.md 登记职责 → proto/接口 → 实现 → 测试 → README
- mongoose 定位:GPLv2(项目开源,已决议);loop 回调内才可碰连接,其他线程一律 `netcore_post()`

**已定关键技术事实(源码核对,勿再猜测)**
- mongoose socket 路径 `read_conn` 每轮 `ioalloc` 把 `c->recv` 按 `MG_IO_SIZE` 扩容继续读,**不消费不会产生 TCP 背压**;`c->recv.len` 顶到 `MG_MAX_RECV_SIZE`(默认 3MB)时 `mg_error` 关连接
- `ota_write_chunk` 满时阻塞 `cv_space`,timedwait 上界 10s;`PIPE_CAP=256KB`
- mongoose 显式 WS 升级 = `mg_ws_upgrade(c, hm, NULL)`;头先至事件 = `MG_EV_HTTP_HDRS`;跨线程唤醒 = `mg_wakeup_init`+`mg_wakeup`;定时器 = `mg_timer_add`

---

### Task 1: netcore 模块骨架(mongoose 接入 + loop 线程 + post + 心跳)

**Files:**
- Create: `door-guard/modules/net/netcore.h`、`door-guard/modules/net/netcore.c`
- Create: `door-guard/tests/test_netcore.c`
- Modify: `door-guard/CMakeLists.txt`(mongoose 静态库目标;netcore.c 进 dg_net)
- Modify: `door-guard/tests/CMakeLists.txt`(注册 test_netcore)
- Modify: `door-guard/app/main.c`(holder 表注册 netcore,置于 web/ntp/mdns 之前)

**Interfaces (Produces):**
```c
int     netcore_start(void);   /* DG_OK / DG_ERR_IO(线程创建失败) */
void    netcore_stop(void);    /* 幂等;join loop 线程 */
bool    netcore_running(void);
int64_t netcore_heartbeat_ms(void);
void    netcore_post(void (*fn)(void *arg), void *arg); /* 慢满则丢最旧并计数 */
struct mg_mgr *netcore_mgr(void); /* 仅 start~stop 之间(装配期)可用 */
```

- [ ] **Step 1: CMake 接入 mongoose**(`door-guard/CMakeLists.txt`,civetweb 段之前插入)

```cmake
# mongoose:第三方网络库(GPLv2,单文件 vendored;统一网络层,见 modules/net/netcore)
add_library(mongoose STATIC third_party/mongoose/mongoose.c)
target_include_directories(mongoose PUBLIC third_party/mongoose)
target_compile_definitions(mongoose PRIVATE MG_ARCH=MG_ARCH_UNIX MG_ENABLE_TCPIP=0)
```

- [ ] **Step 2: 写 netcore.h**(完整内容,注释写明线程契约)

```c
/*
 * netcore.h — 统一网络事件循环(modules/net;mongoose 胶水,零业务)
 *
 * 线程契约(违反即数据竞争):
 *   1. 全部网络连接只在 loop 线程读写;MG_EV_* 回调里可直接调 mongoose API
 *   2. 其他线程(总线/UI/业务)绝不碰连接,投递一律 netcore_post()
 *   3. netcore_mgr() 仅限 start~stop 之间的装配期(注册监听/定时器)
 */
#ifndef DG_NETCORE_H
#define DG_NETCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "mongoose.h"

#ifdef __cplusplus
extern "C" {
#endif

int     netcore_start(void);
void    netcore_stop(void);
bool    netcore_running(void);
int64_t netcore_heartbeat_ms(void);          /* loop 每轮刷新(看门狗判活) */
void    netcore_post(void (*fn)(void *arg), void *arg);
struct mg_mgr *netcore_mgr(void);

#ifdef __cplusplus
}
#endif
#endif /* DG_NETCORE_H */
```

- [ ] **Step 3: 写 netcore.c**(完整实现:loop 线程、post 队列 + socketpair waker、心跳)

要点:post 队列 = 定长 `NETCORE_POST_MAX`(32)槽 `{fn,arg}`,互斥锁保护,满丢最旧计数;
waker 用 `mg_wakeup_init(&mgr, s_wake_fd)` + `mg_wakeup(&mgr,...)`(7.23 接口以 mongoose.h
为准);loop 线程每轮 `mg_mgr_poll(&mgr, 100)` 刷新 `s_hb_ms`;stop 置标志 + 唤醒 + join,
`mg_mgr_free` 收尾。post 在 `!s_running` 时静默丢弃(停服竞态安全)。

- [ ] **Step 4: 写失败测试 tests/test_netcore.c**

```c
/* 用例:1) start 后 heartbeat_ms 单调推进;2) netcore_post 的闭包在 loop 线程执行;
   3) stop 后 running=false 且 post 不崩;4) start/stop 各 3 轮无泄漏 */
```

- [ ] **Step 5: 注册测试**(tests/CMakeLists.txt:`dg_add_test(test_netcore dg_net)` 或直连
  `netcore.c mongoose pthread` 二者取与现有风格一致者)+ `dg-test` 跑通

- [ ] **Step 6: main.c 装配**——holder 表加 `mod_netcore`(`netcore_start()`),排在
  `mod_ntp/mod_web/mod_mdns` 之前;对应 DEP 列表后续任务再挂依赖

- [ ] **Step 7: 宿主全测 + commit**(`feat(netcore): 统一网络事件循环模块骨架`)

### Task 2: web 单会话策略(TDD,civetweb 上先行)

**Files:**
- Modify: `door-guard/tests/test_web_auth.c`(重写 `t_sessions`)
- Modify: `door-guard/services/web/web_session.c`(create 吊销其余)
- Modify: `door-guard/services/web/web_session.h`(注释:单会话语义)
- Modify: `door-guard/tests/web/web_test.sh`(新增"二次登录踢首登录"用例)

**Interfaces (Produces):** `web_session_create()` 语义:签发成功 ⇒ 其余全部失效;
`web_session_count()` 任意时刻 ≤1。API 签名全部不变。

- [ ] **Step 1: 改失败测试**——t_sessions 新断言:两次 create 后 t1 失效、t2 有效、
  count==1;循环登录 3 次,只有最新 token 有效
- [ ] **Step 2: 跑 `dg-test` test_web_auth,确认 FAIL**
- [ ] **Step 3: 实现**——`web_session_create()` 拿锁后先清空全部槽位再落新 token;
  头文件注释同步("同一时刻仅一个有效会话,新登录必胜:崩溃浏览器不会永久占坑")
- [ ] **Step 4: `dg-test` 全绿**
- [ ] **Step 5: web_test.sh 用例**(插在登录用例之后,后续用例改用新 TOKEN):
  `TOKEN2=登录2 → 首TOKEN /api/device == 401 → TOKEN=$TOKEN2`
- [ ] **Step 6: `tests/web/web_test.sh` 全绿 + commit**(`feat(web): 单会话策略——新登录吊销其余会话`)

### Task 3: ota_service 非阻塞余量查询(TDD)

**Files:**
- Modify: `door-guard/services/ota/ota_service.h`、`ota_service.c`
- Modify: `door-guard/tests/test_ota.c`

**Interfaces (Produces):** `size_t ota_can_accept(void);` —— 环形缓冲剩余字节
(`!active` 时 0)。给 web 的 MG_EV_READ 喂入限流用,loop 线程绝不阻塞。

- [ ] **Step 1: test_ota.c 失败用例**:begin 后 `can_accept()==PIPE_CAP`;
  写入 N 字节后 `==PIPE_CAP-N`;abort 后 `==0`
- [ ] **Step 2: 跑 test_ota 确认 FAIL** → **Step 3: 实现**(锁内 `PIPE_CAP - s_ctx.count`)
  → **Step 4: 全绿 + commit**(`feat(ota): ota_can_accept 非阻塞余量查询`)

### Task 4: web/OTA 端点迁移到 mongoose(netcore 之上)

**Files:**
- Modify: `door-guard/services/web/web_server.c`(传输层重写,业务 handler 搬运)
- Modify: `door-guard/services/web/web_server.h`(注释更新;API 不变)
- Modify: `door-guard/services/web/README.md`(线程模型章节)

**Interfaces:** Consumes Task 1 的 netcore.h 全部函数、Task 3 的 `ota_can_accept()`。

**搬运对照表(civetweb 版 → mongoose 版,业务逻辑逐字保留):**

| 现函数 | 处置 |
|---|---|
| handle_login/logout/device/logs/account | handler 体不变;入口签名改 `(mg_connection*, mg_http_message*, ...)`;`mg_read(body)` 改 `hm->body`;`mg_get_var(qs)` 改 `mg_http_get_var(&hm->query,...)`;`mg_get_header` 同名保留 |
| handle_ota | 重写:MG_EV_HTTP_HDRS 校验+ota_begin → MG_EV_READ 按 `min(recv.len, ota_can_accept())` 喂入并 `mg_iobuf_del`;极端慢盘时 recv 涨到 3MB 由 mongoose 报错断开(有界,客户端 X-OTA-Offset 续传) |
| ws_connect/ready/data/close + s_ws 表 | 重写:token 校验失败 `mg_http_reply(401)`+关;成功 `mg_ws_upgrade`;连接表 loop 私有无锁 |
| ws_pusher_thread + 队列 | 线程删除;队列保留;`netcore_post(drain)` 排空 + 100ms `mg_timer_add` 兜底 |
| http_send/json_reply/json_msg | mongoose 版重写(`mg_printf` 手写头,保留 nosniff/no-store);reason_phrase 不变 |
| handle_static/asset_lookup | 原样;`mg_http_match_uri` 精确匹配 + `/api/` 前缀 404 + SPA 回退 |
| on_auth_result/on_ntp_result/publish_web_state/on_web_set | 事件回调不变(它们只入队/发事件,不碰连接) |
| web_server_start/stop | start:web_auth_ensure → netcore_mgr 上 `mg_http_listen` → 订阅总线;stop:退订 → `netcore_stop` → `web_session_revoke_all` |
| web_server_heartbeat_ms | 透传 `netcore_heartbeat_ms()` |

- [ ] **Step 1: 重写 web_server.c**(按对照表;路由表仍以字面量写在源码里,
  `frontend_check.py` 的 endpoints 对照依赖它)
- [ ] **Step 2: `dg-build-pc` 编过(零警告)**
- [ ] **Step 3: `tests/web/web_test.sh` 63 项全绿**;`ws_test.py` 全绿;
  `frontend_check.py` 全绿
- [ ] **Step 4: 并发用例**:OTA 上传中打 `/api/device` 200(web_test.sh 追加)
- [ ] **Step 5: commit**(`refactor(web): web 上位机迁移 mongoose 统一事件循环`)

### Task 5: civetweb 退役

- [ ] **Step 1:** 删 `third_party/civetweb/`(civetweb.c 22.5k 行 + shim 头 + LICENSE);
  CMake 删 civetweb 目标与 `-include` shim、`dg_net` 改链 `mongoose`、去 `crypto`
  (mongoose 自带 SHA1;确认无他人引用 crypto 再删)
- [ ] **Step 2:** `dg-build-pc` + 全宿主测试(web/auth/ota)全绿
- [ ] **Step 3:** `source env/env.sh && dg-build` 交叉零警告
- [ ] **Step 4:** commit(`refactor(third_party): civetweb 退役,统一 mongoose`)

### Task 6: mDNS 迁移到 loop

**Files:** Modify `services/mdns/mdns_responder.c`(+README);接口 `mdns_start/mdns_running/
mdns_hostname/mdns_url` 不变。

**保留(逐字):** `handle_query`、`check_conflict`、`send_probe`、`send_announce`、
`send_goodbye`、`send_response`、`refresh_ip`、`snapshot`、mdns_wire.c 全部。
**替换:** `mdns_thread`+`open_socket`+`join_all_interfaces`+自管 recv 循环 →
`mg_open_listener("udp://0.0.0.0:5353")` + `c->fd` 上组播加入/`IP_MULTICAST_LOOP=0` +
`mg_timer_add` 状态机步进(ST_PROBING/ST_ANNOUNCING 的节拍);`bind_iface` 逐网口 egress
若 mongoose `mg_send` 无法携带 ifindex → 每网口一个 listener,以 `mdns_test.sh` 实测裁决。

- [ ] **Step 1: 迁移实现** → **Step 2: `test_mdns_wire` + `tests/web/mdns_test.sh` 全绿**
  → **Step 3: 交叉编译零警告** → **Step 4: commit**(`refactor(mdns): 并入统一事件循环`)

### Task 7: NTP 换应用内 SNTP

**Files:** Modify `services/ntp/ntp_service.c`(+README);`ntp_service.h` API 不变。

- `probe_online()` 的 `system("ping")` → `net_info_is_online()`
- chronyc 外调 → `mg_sntp_connect(netcore_mgr(), ntp_server, cb)`(loop 内异步,
  8s 超时);`MG_EV_SNTP_TIME` → `settimeofday`(UTC)→ publish(ok=true,ts);
  超时/失败 → publish(false) —— EV_NET_NTP_RESULT 契约不变,上位机 202+WS、
  菜单按钮、开机自动一次三路径无感
- 触发从总线线程进入 → `netcore_post` 包一层;`EV_NET_NTP_TRIGGER` 处理器不再 spawn 线程
- 测试:`tests/web/sntp_stub.py` 本地 SNTP 服务器(回显可控),
  `ntp_server` 指向 127.0.0.1(config 注入,仅测试);web_test.sh NTP 组全绿
- [ ] Steps: 实现 → 宿主全测 → 交叉编译 → commit(`feat(ntp): 应用内 SNTP,去 chrony 依赖`)

### Task 8: 清理与文档

- [ ] 删 `ota_port` 死配置:`cfg.h`、`cfg.c`(映射表/默认值/export)、
  `configs/default.json`、`test_cfg.c`;`dg-test` 全绿
- [ ] `spec-network.md`:§1 civetweb→mongoose/netcore、§2.2 监听形态对齐(8080 同端口,
  ota_port 已删)、§3 chrony→SNTP(+rootfs 停用 chrony 说明、步进语义)
- [ ] `references/architecture.md`:模块登记 netcore(职责/线程契约)+ 目录索引;
  `docs/architecture-v2-proposal.md` §3.3 线程归属表同步
- [ ] `third_party/mongoose/README.md`:标记"已接入"+ GPLv2 决议记录;
  `door-guard/README.md` 目录职责;`services/{web,mdns,ntp}/README.md` 线程模型章节
- [ ] commit(`docs+chore: 统一网络层落地收尾,删 ota_port 死配置`)

### Task 9: 全量验收与交付

- [ ] 宿主全量:`dg-test` 全绿 + `tests/web/web_test.sh`、`ws_test.py`、`mdns_test.sh`、
  `frontend_check.py`、前端 vitest 44 项全绿
- [ ] `dg-build` 零警告
- [ ] 若 `DOORGUARD_IP` 可达:`dg-deploy` 推板 + 板上冒烟(服务起、页面开);
  否则记录"板上人工验收待做"(双网口 mDNS、OTA 续传、并发推送、两浏览器互踢、看门狗)
- [ ] DEVLOG 顶部追加条目(≤30 行)→ commit + push origin master
