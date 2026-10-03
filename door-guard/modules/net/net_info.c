/*
 * net_info.c — 网络接口只读信息实现
 *
 * 注意点(踩过/易错处):
 *   1. 只认 AF_INET(不碰 IPv6):门禁局域网访问走 IPv4,mDNS A 记录同理;
 *   2. 必须跳过 IFF_LOOPBACK,**并且**要求 IFF_UP——接口 DOWN 时
 *      getifaddrs 仍可能带旧地址(实测 eth0 手动 up 前就是这状态);
 *   3. 169.254.x.x 是链路本地自配地址(无 DHCP 时的残值):通告/显示它
 *      只会让用户白等,直接排除;
 *   4. 外网可达性用 ping 外探(与 ntp_service 同一判据),结果缓存,
 *      避免每次 UI 刷新都 fork 一次 ping;
 *   5. 网关读 /proc/net/route 而不是 netlink dump:内核该表永远反映真实
 *      路由,DHCP/静态两条配网路都覆盖,纯文本解析零依赖(2026-09-27)。
 */
#include "net_info.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NET_ONLINE_CACHE_S 15
#define NET_ONLINE_PROBE   "ping -c1 -W2 223.5.5.5 > /dev/null 2>&1"

#define NET_ZERO_ADDR "0.0.0.0"

static int find_primary(char *ip, size_t ip_cap, char *name, size_t name_cap,
                        char *mask, size_t mask_cap, bool *link_up)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return DG_ERR_IO;

    int rc = DG_ERR_NOT_FOUND;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if ((p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_in *in = (struct sockaddr_in *)p->ifa_addr;
        if (in->sin_addr.s_addr == htonl(INADDR_ANY))
            continue;
        uint32_t host = ntohl(in->sin_addr.s_addr);
        if ((host & 0xFFFF0000u) == 0xA9FE0000u)
            continue;
        char tmp[64];
        if (inet_ntop(AF_INET, &in->sin_addr, tmp, sizeof(tmp)) == NULL)
            continue;
        if (ip && ip_cap)
            snprintf(ip, ip_cap, "%s", tmp);
        if (name && name_cap)
            snprintf(name, name_cap, "%s", p->ifa_name);
        if (link_up)
            *link_up = !!(p->ifa_flags & IFF_LOWER_UP);
        if (mask && mask_cap) {
            /* 地址与 netmask 是同名接口的两个条目;取不到(异常)给 0.0.0.0 */
            if (p->ifa_netmask && p->ifa_netmask->sa_family == AF_INET)
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)p->ifa_netmask)->sin_addr,
                          mask, mask_cap);
            else
                snprintf(mask, mask_cap, "%s", NET_ZERO_ADDR);
        }
        rc = DG_OK;
        break;
    }
    freeifaddrs(ifa);
    return rc;
}

int net_info_primary_ipv4(char *out, size_t cap)
{
    if (!out || cap == 0)
        return DG_ERR_PARAM;
    return find_primary(out, cap, NULL, 0, NULL, 0, NULL);
}

int net_info_primary_ifname(char *out, size_t cap)
{
    if (!out || cap == 0)
        return DG_ERR_PARAM;
    return find_primary(NULL, 0, out, cap, NULL, 0, NULL);
}

/* 主接口的默认网关(点分);无默认路由 → DG_ERR_NOT_FOUND。
 * /proc/net/route 每行:接口名 + 小端 hex 的目的/网关列;dest=0 即默认路由 */
static int read_default_gw(const char *ifname, char *out, size_t cap)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return DG_ERR_IO;

    char line[256];
    int rc = DG_ERR_NOT_FOUND;
    while (fgets(line, sizeof(line), f)) {
        char ifc[64];
        uint32_t dest = 0, gw = 0;
        if (sscanf(line, "%63s %x %x", ifc, &dest, &gw) != 3)
            continue;
        if (dest != 0 || strcmp(ifc, ifname) != 0)
            continue;
        struct in_addr a = { .s_addr = gw };   /* 内核导出的就是主机序布局 */
        if (inet_ntop(AF_INET, &a, out, cap)) {
            rc = DG_OK;
            break;
        }
    }
    fclose(f);
    return rc;
}

int net_info_read(net_info_addr_t *out)
{
    if (!out)
        return DG_ERR_PARAM;
    memset(out, 0, sizeof(*out));
    snprintf(out->ip, sizeof(out->ip), "%s", NET_ZERO_ADDR);
    snprintf(out->mask, sizeof(out->mask), "%s", NET_ZERO_ADDR);
    snprintf(out->gw, sizeof(out->gw), "%s", NET_ZERO_ADDR);

    /* 没拿到地址不算错:保持 0.0.0.0 兜底(have_ip=false),展示层直接渲染 */
    if (find_primary(out->ip, sizeof(out->ip),
                     out->ifname, sizeof(out->ifname),
                     out->mask, sizeof(out->mask),
                     &out->have_link) != DG_OK)
        return DG_OK;
    out->have_ip = true;
    (void)read_default_gw(out->ifname, out->gw, sizeof(out->gw));
    return DG_OK;
}

bool net_info_is_online(void)
{
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    static bool cached = false;
    static time_t cached_at = 0;

    char ip[64];
    if (net_info_primary_ipv4(ip, sizeof(ip)) != DG_OK)
        return false;                        /* 连 IP 都没有:必然未联网 */

    time_t now = time(NULL);
    pthread_mutex_lock(&mtx);
    if (cached_at != 0 && (now - cached_at) < NET_ONLINE_CACHE_S) {
        bool v = cached;
        pthread_mutex_unlock(&mtx);
        return v;
    }
    pthread_mutex_unlock(&mtx);

    bool online = (system(NET_ONLINE_PROBE) == 0);

    pthread_mutex_lock(&mtx);
    cached = online;
    cached_at = now;
    pthread_mutex_unlock(&mtx);
    return online;
}
