/*
 * mdns_responder.h — mDNS 应答器(局域网按名字访问:doorguard.local + _http._tcp)
 *
 * 为什么自实现:avahi 不在 B4 rootfs(spec-network §1),而门禁只需要
 * "局域网内按名字找到设备"这一件事——完整服务发现栈(缓存/代理/冲突
 * 重命名循环)属过度设计。本模块覆盖 RFC 6762 的必要子集:
 *   探测(3 次,防重名)→ 通告(2 次)→ 应答查询(含 legacy 单播)→ 关机 goodbye
 * 同时公告 http 服务(PTR/SRV/TXT),手机 mDNS 浏览器与 avahi-browse 可直接发现。
 */
#ifndef DG_MDNS_RESPONDER_H
#define DG_MDNS_RESPONDER_H

#include <stdbool.h>
#include <stddef.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动应答器:
 *   主机名取 device_config "mdns_host"(默认 doorguard),服务端口取
 *   device_config web_port(默认 8080)。
 * 失败(5353 被占/组播入组失败)返回错误码,不阻塞其余业务。
 */
int mdns_start(void);

/** 停止:发 goodbye(TTL=0)后关 socket */
void mdns_stop(void);

/** 是否运行中 */
bool mdns_running(void);

/** 本机通告的主机名(不含 .local),如 "doorguard" */
int mdns_hostname(char *out, size_t cap);

/** 局域网访问地址,如 "http://doorguard.local:8080"(UI/web 展示用) */
int mdns_url(char *out, size_t cap);

/** 手动重新通告(端口/主机名改动后调用):立即发一轮 announce 刷新局域网缓存 */
int mdns_announce(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_MDNS_RESPONDER_H */
