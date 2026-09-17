/*
 * mdns_responder.c — 轻量 mDNS A 记录应答器
 *
 * 只实现最小闭环:监听 224.0.0.251:5353,对查询 "doorguard.local"(或本机
 * hostname.local)回 A 记录 = 本机 eth0/eth1/wlan0 的首个 IPv4。
 * 完整 mDNS(服务发现/冲突处理)不在范围——门禁只需要"可解析"。
 */
#include "mdns_responder.h"
#include "dg_log.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *TAG = "[MDNS]";

#define MDNS_HOST "doorguard"
#define MDNS_DOMAIN ".local"
#define MDNS_PORT 5353

static int s_sock = -1;
static pthread_t s_tid;
static bool s_running = false;

/* 取首个非 lo 的 IPv4 */
static int get_local_ip(uint8_t out[4])
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return DG_ERR_IO;
    int rc = DG_ERR_NOT_FOUND;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (p->ifa_flags & IFF_LOOPBACK)
            continue;
        struct sockaddr_in *in = (struct sockaddr_in *)p->ifa_addr;
        memcpy(out, &in->sin_addr.s_addr, 4);
        rc = DG_OK;
        break;
    }
    freeifaddrs(ifa);
    return rc;
}

/* 组装 A 记录应答(DNS 报文:头 12B + 原 query + 压缩指针应答) */
static size_t build_answer(const uint8_t *query, size_t qlen, uint8_t ip[4],
                           uint8_t *out, size_t cap)
{
    if (qlen + 16 > cap)
        return 0;
    memcpy(out, query, qlen);
    size_t o = qlen;
    out[o++] = 0x00;                        /* 名字结束(查询已含;为简化按原样+终止) */
    /* 简化:报文头由调用方构造,这里追加 type A(0x0001) class IN(0x0001)
     * TTL 120 rdlength 4 rdata=ip —— 固定 12 字节应答记录 */
    static const uint8_t rec[] = { 0x00, 0x01, 0x00, 0x01,
                                   0x00, 0x00, 0x00, 0x78,
                                   0x00, 0x04 };
    memcpy(out + o, rec, sizeof(rec));
    o += sizeof(rec);
    memcpy(out + o, ip, 4);
    o += 4;
    return o;
}

static int name_matches(const uint8_t *q, size_t qlen)
{
    /* 查询名:<len>doorguard<len>local 0x00 */
    static const char pat[] = { 9, 'd', 'o', 'o', 'r', 'g', 'u', 'a', 'r', 'd',
                                5, 'l', 'o', 'c', 'a', 'l' };
    if (qlen < sizeof(pat))
        return 0;
    return memcmp(q, pat, sizeof(pat)) == 0;
}

static void *mdns_thread(void *arg)
{
    (void)arg;
    uint8_t buf[1500];
    while (s_running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(s_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 12)
            continue;
        uint16_t flags = ((uint16_t)buf[2] << 8) | buf[3];
        if ((flags & 0x8000) == 0)
            continue;                       /* 只应答 QR=1(查询) */

        /* 问题段从第 12 字节开始 */
        if (!name_matches(buf + 12, (size_t)n - 12))
            continue;

        uint8_t ip[4];
        if (get_local_ip(ip) != DG_OK)
            continue;

        /* 构造应答:原头(QR=1, AA=1)+ 问题 + 答案 */
        uint8_t resp[1500];
        memcpy(resp, buf, (size_t)n);
        resp[2] = 0x84;                     /* QR=1 AA=1 */
        resp[3] = 0x00;
        resp[6] = 0; resp[7] = 1;           /* ANCOUNT=1 */
        size_t qlen = 12;
        while (qlen < (size_t)n && buf[qlen] != 0)
            qlen += buf[qlen] + 1;
        qlen++;                             /* 含终止 0 */
        size_t alen = build_answer(buf + 12, qlen - 12, ip,
                                   resp + n, sizeof(resp) - n);
        /* 注:问题段已在原报文中,答案追加在报文尾(名字用原样展开,未压缩) */
        (void)alen;
        sendto(s_sock, resp, n + 16, 0, (struct sockaddr *)&peer, plen);
    }
    return NULL;
}

int mdns_start(void)
{
    if (s_running)
        return DG_OK;

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0)
        return DG_ERR_IO;

    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct ip_mreq mreq;
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        DG_LOGW(TAG, "bind 5353 失败(其他 mdns 服务在跑?)");
        close(s_sock);
        s_sock = -1;
        return DG_ERR_IO;
    }

    s_running = true;
    if (pthread_create(&s_tid, NULL, mdns_thread, NULL) != 0) {
        close(s_sock);
        s_sock = -1;
        s_running = false;
        return DG_ERR_INTERNAL;
    }
    DG_LOGI(TAG, "doorguard.local 通告中");
    return DG_OK;
}

void mdns_stop(void)
{
    if (!s_running)
        return;
    s_running = false;
    close(s_sock);
    s_sock = -1;
}
