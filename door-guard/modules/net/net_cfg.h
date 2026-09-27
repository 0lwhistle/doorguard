/*
 * net_cfg.h — 网络配置应用(静态 IP / DHCP;2026-09-27 web 上位机网络配置)
 *
 * 职责:把 device_config 里的 net_mode/net_ip/net_mask/net_gw 落到主接口上。
 * 读取(实际地址)在 net_info;本模块只管"应用期望配置"。
 *
 * 为什么走 shell 而不是 ioctl:静态/DHCP 两态都要与 dhcpcd 协调(先 -x 释放、
 * 回 DHCP 要 -n 重新纳入管理),ip/dhcpcd 命令是唯一同时覆盖这两件事的路径;
 * 应用入口只有 web(单线程触发)与开机装配,阻塞数百 ms 可接受。
 *
 * 线程契约:net_cfg_apply 阻塞(system,dhcpcd 交互),**不得**在 netcore loop
 * 或 UI 线程调用——web 侧放独立线程,开机装配在 main(应用起来前)同步调。
 */
#ifndef DG_NET_CFG_H
#define DG_NET_CFG_H

#include <stdbool.h>
#include <stddef.h>

#include "err.h"
#include "net_info.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次配置请求(mode + 三地址;gw 空串 = 不下发默认路由) */
typedef struct {
    bool is_static;                       /**< false = 回 DHCP */
    char ip[16];
    char mask[16];
    char gw[16];
} net_cfg_req_t;

/** 点分掩码 → 前缀长度(1~32);格式错/非连续掩码 → DG_ERR_PARAM */
int net_cfg_mask_plen(const char *mask);

/** 请求合法性:DHCP 恒过;静态要求 ip/mask 合法且 mask 连续、gw 空或合法 */
int net_cfg_validate(const net_cfg_req_t *r);

/**
 * 应用配置到主接口(阻塞,数秒内返回)。成功后 out_result 带应用后的实际
 * 地址(可为 NULL)。接口不存在/命令失败 → 相应错误码。
 */
int net_cfg_apply(const net_cfg_req_t *r, net_info_addr_t *out_result);

/**
 * 开机装配:cfg net_mode=static 时按持久化配置应用一次;DHCP(默认)不动
 * ——开机配网仍由 rootfs 的 S41dhcpcd 负责,应用只在用户明确选静态时接管。
 */
int net_cfg_apply_saved(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_NET_CFG_H */
