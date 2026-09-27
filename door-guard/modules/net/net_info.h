/*
 * net_info.h — 网络接口只读信息(web 上位机 / UI 设备页 / mDNS 共用)
 *
 * 为什么独立成模块:板上接口名不固定(eth0/eth1/wlan0,见 DEV_HANDBOOK §8),
 * "取哪个 IP"这条规则若各写各的,mDNS 通告的地址、UI 显示的地址、web 状态
 * 会互相打架。规则只在这里实现一次。
 *
 * 语义:primary = 首个非环回、UP、已拿到 IPv4 的接口。
 * 展示兜底:任何一项取不到都以 "0.0.0.0" 呈现(2026-09-27 网络配置需求:
 * 上位机/主页/设备页统一占位,不再各写各的 "-" 或空串)。
 */
#ifndef DG_NET_INFO_H
#define DG_NET_INFO_H

#include <stdbool.h>
#include <stddef.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 主接口 IPv4 点分字符串(如 "192.168.2.95");无可用接口 → DG_ERR_NOT_FOUND */
int net_info_primary_ipv4(char *out, size_t cap);

/** 主接口名(如 "eth0");无 → DG_ERR_NOT_FOUND */
int net_info_primary_ifname(char *out, size_t cap);

/** 是否已联网(有非环回 IPv4 且能通外网;探测结果缓存 15s)
 *  注意:内部走 ping,首次调用可能阻塞 ~2s,勿在 UI 渲染热路径里裸调 */
bool net_info_is_online(void);

/** 主接口地址快照(IP + 子网掩码 + 默认网关,一次读齐)
 *
 *  网关取 /proc/net/route 中主接口的默认路由;没有默认路由 → "0.0.0.0"。
 *  无主接口(没插网线/未拿到地址)时不报错:返回 DG_OK 且全部填 "0.0.0.0"、
 *  have_ip=false —— 展示层直接渲染,不必再各自兜底。 */
typedef struct {
    char ifname[16];
    char ip[16];
    char mask[16];
    char gw[16];
    bool have_ip;
} net_info_addr_t;

int net_info_read(net_info_addr_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DG_NET_INFO_H */
