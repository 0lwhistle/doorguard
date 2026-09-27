/*
 * net_cfg.c — 网络配置应用实现(静态 IP / DHCP)
 *
 * 与 dhcpcd 的协调(板上实测:dhcpcd 常驻,S41dhcpcd 开机拉起):
 *   - 静态:先 `dhcpcd -x <if>` 释放并把接口移出 dhcpcd 管理——否则 dhcpcd
 *     会按租约周期覆盖掉手动地址;失败容忍(dhcpcd 未跑时 -x 报 not running)。
 *   - 回 DHCP:清掉静态残留后 `dhcpcd -n <if>` 触发重新获取;主进程不在
 *     (异常态)才补一个后台实例。
 * 全部地址参数过 inet_pton 校验后才拼命令,无注入面。
 */
#include "net_cfg.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cfg.h"
#include "dg_log.h"
#include "net_info.h"

static const char *TAG = "[NETCFG]";

/* 互斥:web 触发与开机装配并发时排队而非交叉下命令 */
static pthread_mutex_t s_apply_mu = PTHREAD_MUTEX_INITIALIZER;

/* 点分 → in_addr;仅 AF_INET 点分十进制(拒绝 "1.2.3" 等缩写,inet_aton 收
 * 十六进制/缩写,这里用 inet_pton 收紧) */
static bool parse_ipv4(const char *s, struct in_addr *out)
{
    if (!s || !s[0])
        return false;
    return inet_pton(AF_INET, s, out) == 1;
}

int net_cfg_mask_plen(const char *mask)
{
    struct in_addr a;
    if (!parse_ipv4(mask, &a))
        return DG_ERR_PARAM;
    uint32_t v = ntohl(a.s_addr);
    if (v == 0)
        return DG_ERR_PARAM;                 /* 0.0.0.0 不是合法掩码 */
    /* 连续性:形如 1..1 0..0(1 的个数在前);(v+1) 是把尾部 0 全变 1 */
    if ((v | (v + 1)) != 0xFFFFFFFFu)
        return DG_ERR_PARAM;
    int n = 0;
    while (v) {
        v <<= 1;
        n++;
    }
    return n;
}

int net_cfg_validate(const net_cfg_req_t *r)
{
    if (!r)
        return DG_ERR_PARAM;
    if (!r->is_static)
        return DG_OK;                        /* DHCP 不带地址参数 */

    struct in_addr a;
    if (!parse_ipv4(r->ip, &a))
        return DG_ERR_PARAM;
    uint32_t host = ntohl(a.s_addr);
    if (host == 0 || (host >> 24) == 127 || (host & 0xFFFF0000u) == 0xA9FE0000u)
        return DG_ERR_PARAM;                 /* 0.0.0.0 / 环回 / 链路本地无意义 */
    if (net_cfg_mask_plen(r->mask) < 1)
        return DG_ERR_PARAM;
    if (r->gw[0]) {                          /* 网关可选:空 = 不配默认路由 */
        if (!parse_ipv4(r->gw, &a) || a.s_addr == 0)
            return DG_ERR_PARAM;
    }
    return DG_OK;
}

/* 应用目标接口:第一个非环回且 UP 的接口——**不要求已拿到地址**。
 * 静态配置的首次应用/开机恢复恰恰发生在"接口还没地址"时(net_info 的
 * primary 规则要求有 IPv4,那条规则是给展示用的,不适用于配置写入) */
static int pick_ifname(char *out, size_t cap)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return DG_ERR_IO;
    int rc = DG_ERR_NOT_FOUND;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_name)
            continue;
        if ((p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP))
            continue;
        snprintf(out, cap, "%s", p->ifa_name);
        rc = DG_OK;
        break;
    }
    freeifaddrs(ifa);
    return rc;
}

/* 应用主体(持锁调用)。成功返回 DG_OK 并经 out_result 回实际地址 */
static int apply_locked(const net_cfg_req_t *r, net_info_addr_t *out_result)
{
    char ifname[16];
    if (pick_ifname(ifname, sizeof(ifname)) != DG_OK) {
        DG_LOGW(TAG, "无可用接口(网线未插?),配置不生效");
        return DG_ERR_NOT_FOUND;
    }

    char cmd[256];
    if (r->is_static) {
        int plen = net_cfg_mask_plen(r->mask);
        if (plen < 1)
            return DG_ERR_PARAM;
        /* dhcpcd -x 释放租约并移出管理(未跑时容忍失败);掩码以前缀写入 */
        snprintf(cmd, sizeof(cmd),
                 "dhcpcd -x %s >/dev/null 2>&1; "
                 "ip addr flush dev %s; "
                 "ip addr add %s/%d dev %s && ip link set %s up",
                 ifname, ifname, r->ip, plen, ifname, ifname);
        if (r->gw[0])
            snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                     " && ip route replace default via %s dev %s", r->gw, ifname);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "dhcpcd -x %s >/dev/null 2>&1; "
                 "ip addr flush dev %s; ip link set %s up; "
                 "if pidof dhcpcd >/dev/null 2>&1; then dhcpcd -n %s; "
                 "else dhcpcd -b -q %s >/dev/null 2>&1; fi",
                 ifname, ifname, ifname, ifname, ifname);
    }

    DG_LOGI(TAG, "应用网络配置(%s %s): %s",
            r->is_static ? "static" : "dhcp", ifname, cmd);
    int st = system(cmd);
    if (st != 0) {
        DG_LOGW(TAG, "配置命令失败(exit=%d)", st);
        return DG_ERR_NETWORK;
    }

    /* 回读实际生效地址,顺手把结果交给调用方发事件 */
    net_info_addr_t got;
    net_info_read(&got);
    if (out_result)
        *out_result = got;
    if (r->is_static && strcmp(got.ip, r->ip) != 0) {
        DG_LOGW(TAG, "静态地址未生效(现 %s)", got.ip);
        return DG_ERR_NETWORK;
    }
    return DG_OK;
}

int net_cfg_apply(const net_cfg_req_t *r, net_info_addr_t *out_result)
{
    int vrc = net_cfg_validate(r);
    if (vrc != DG_OK)
        return vrc;

    pthread_mutex_lock(&s_apply_mu);
    int rc = apply_locked(r, out_result);
    pthread_mutex_unlock(&s_apply_mu);
    return rc;
}

int net_cfg_apply_saved(void)
{
    const dg_cfg_t *cfg = cfg_get();
    if (!cfg || strcmp(cfg->net_mode, "static") != 0)
        return DG_OK;                        /* dhcp/未知:交给 S41dhcpcd */

    net_cfg_req_t r;
    memset(&r, 0, sizeof(r));
    r.is_static = true;
    snprintf(r.ip, sizeof(r.ip), "%s", cfg->net_ip);
    snprintf(r.mask, sizeof(r.mask), "%s", cfg->net_mask);
    snprintf(r.gw, sizeof(r.gw), "%s", cfg->net_gw);
    DG_LOGI(TAG, "开机恢复静态配置(%s/%s 网关 %s)", r.ip, r.mask,
            r.gw[0] ? r.gw : "无");
    return net_cfg_apply(&r, NULL);
}
