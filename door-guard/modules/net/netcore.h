/*
 * netcore.h — 统一网络事件循环(modules/net;mongoose 胶水,零业务)
 *
 * 承载全部网络 I/O:web 上位机(HTTP+WS+OTA 上传端点)、mDNS(UDP)、
 * NTP(SNTP)。一个 loop 线程独占所有连接的读写——mongoose 连接不是
 * 线程安全的,跨线程碰连接是数据竞争,这条契约是整个网络层的地基。
 *
 * 线程契约(违反即数据竞争,详见模块 README):
 *   1. MG_EV_* 回调在 loop 线程执行,回调里可直接调 mongoose API
 *   2. 其他线程(总线/UI/业务)绝不碰连接,投递一律 netcore_post()
 *      ——闭包在 loop 线程执行,因此闭包内做 mg_http_listen 等
 *      注册操作也是安全的(注册与 poll 同样不许并发)
 *   3. loop 内回调不允许阻塞(OTA 喂入、SNTP 全是事件驱动)
 */
#ifndef DG_NETCORE_H
#define DG_NETCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "err.h"
#include "mongoose.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 loop 线程(幂等)。注册动作经 netcore_post 投递,不要在别的线程调 mg_* */
int netcore_start(void);

/** 停轮、释放全部连接、join 线程(幂等) */
void netcore_stop(void);

bool netcore_running(void);

/** loop 每轮刷新;看门狗经 web_server_heartbeat_ms 间接消费 */
int64_t netcore_heartbeat_ms(void);

/**
 * 任意线程 → loop 线程执行闭包。队列满丢最旧并计数(实时事件宁可丢
 * 不可堵总线,与 web WS 队列同一策略);停服后静默丢弃。
 * 闭包内不得再无限投递自己(排水有轮数上界,防饿死 loop)。
 */
void netcore_post(void (*fn)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif
#endif /* DG_NETCORE_H */
