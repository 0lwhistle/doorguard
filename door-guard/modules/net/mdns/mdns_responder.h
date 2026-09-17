/*
 * mdns_responder.h — mDNS 通告(轻量自实现;avahi 不在 B4 rootfs)
 *
 * 应答 doorguard.local 的 A 记录查询(组播 224.0.0.251:5353)。
 * WSL2 NAT 环境组播可能不可达,届时在 WSL /etc/hosts 兜底(README 记录)。
 */
#ifndef DG_MDNS_RESPONDER_H
#define DG_MDNS_RESPONDER_H

#include <stdbool.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int mdns_start(void);
void mdns_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_MDNS_RESPONDER_H */
