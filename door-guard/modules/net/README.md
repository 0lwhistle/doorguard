# modules/net — 网络模块(net_info + netcore)

| 文件 | 职责 |
|---|---|
| `net_info.c/h` | 网口只读信息:主网口 IPv4/接口名/在线状态;UI 网络图标与 web 共用 |
| `netcore.c/h` | **统一网络事件循环**(mongoose 7.23 胶水层,零业务) |

## netcore 是什么

web 上位机(HTTP+WS+OTA 收包)、mDNS(UDP)、NTP(SNTP)的全部网络 I/O
跑在**一个**事件循环线程上。mongoose 连接不是线程安全的,这个模块存在的
意义就是把"谁在什么时候可以碰连接"变成结构约束而不是纪律约束。

## 线程契约(违反即数据竞争)

1. 所有连接只在 **loop 线程**读写;`MG_EV_*` 回调里可直接调 mongoose API。
2. 其他线程(总线/UI/业务)绝不碰连接,投递一律 `netcore_post(fn, arg)`
   ——闭包在 loop 线程执行,所以监听/定时器注册也走它(注册与 poll 同样
   不许并发)。
3. `netcore_mgr()` 仅限 loop 线程(post 闭包内 / MG_EV 回调内)。
4. loop 内回调禁止阻塞:OTA 收包按 `ota_can_accept()` 限流喂入,SNTP 是
   异步事件;秒级操作(如 chronyc 类外呼)在这个架构下没有容身之地。
5. 心跳:loop 每轮刷新 `netcore_heartbeat_ms()`;看门狗经
   `web_server_heartbeat_ms()` 透传消费。`s_running`/`s_hb_ms` 为 C11
   原子量,TSAN 零报告。

## 接口

```c
int netcore_start(void);      // main.c holder 表装配(先于网络服务族)
void netcore_stop(void);
bool netcore_running(void);
int64_t netcore_heartbeat_ms(void);
void netcore_post(void (*fn)(void *), void *arg);   // 满丢最旧;停服静默丢弃
struct mg_mgr *netcore_mgr(void);                   // 仅 loop 线程
```

## 使用示例(在 loop 线程注册监听)

```c
static void setup(void *arg) {
    struct mg_connection *c =
        mg_http_listen(netcore_mgr(), "http://0.0.0.0:8080", my_handler, NULL);
    mg_timer_add(netcore_mgr(), 200, MG_TIMER_REPEAT, my_tick, NULL);
}
int my_service_start(void) {
    if (!netcore_running()) return DG_ERR_IO;
    netcore_post(setup, NULL);
    return DG_OK;
}
```

真实用例:`services/web/web_server.c`(HTTP/WS/OTA)、`services/mdns/mdns_responder.c`
(UDP + 定时器)、`services/ntp/ntp_service.c`(SNTP)。测试:`tests/test_netcore.c`。

## 为什么不用 mg_wakeup 做瞬时唤醒

netcore_post 用 10ms 周期定时器排水(队列空时开销可忽略)。验证类事件
(WS 推送/NTP 触发)对 10ms 延迟不敏感,不值得为瞬时唤醒引入跨线程唤醒
管道;延迟敏感需求出现时再升级。
