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
 *      避免每次 UI 刷新都 fork 一次 ping。
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

static int find_primary(char *ip, size_t ip_cap, char *name, size_t name_cap)
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
    return find_primary(out, cap, NULL, 0);
}

int net_info_primary_ifname(char *out, size_t cap)
{
    if (!out || cap == 0)
        return DG_ERR_PARAM;
    return find_primary(NULL, 0, out, cap);
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
