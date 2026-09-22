/*
 * mdns_responder.c — mDNS 应答器实现(RFC 6762 必要子集)
 *
 * 传输层(2026-09-22 起)挂在 modules/net/netcore 的统一事件循环上,
 * 不再自持线程与 socket:收包 = UDP 监听连接的 MG_EV_READ(每数据报一次,
 * 发送方地址在 c->rem);定时节拍 = mg_timer_add;启停 = netcore_post 闭包。
 *
 * 状态机(逻辑与原独立线程版一致):
 *   PROBING    探测:发 3 次 type=ANY 查询,确认名字没被占用;
 *              期间收到"同名但不同 IP"的 A 应答 → 改名 doorguard-2 再探
 *   ANNOUNCING 通告:发 2 次推送式应答,让局域网填好缓存
 *   READY      常态:应答查询;每 5s 查 IP,变了立即重新通告
 *
 * 应答策略(为什么这样:局域网"按名字访问"的成败全在这几处):
 *   - 查询来自 5353 且未置 QU 位 → 组播应答(带 cache-flush 位)
 *   - 来自其它端口(legacy resolver)或置了 QU 位 → 单播应答:
 *     原样回带问题段 + 回带查询 ID,TTL 压到 10s(不进组播缓存)
 *   - SRV 的 target A 记录放 additional 段:客户端一次拿全,少一轮往返
 *   - SRV/TXT 同时公告 → 手机与 avahi-browse 能直接"发现"设备,不只是解析
 *
 * 接口绑定(相对旧版的已知差异):旧版用 IP_PKTINFO 识别"包从哪个网口来"
 * 并从原网口应答;mongoose 收包不透出入接口,现统一从 net_info 主网口发出。
 * 单 eth 场景行为一致;双网口同时收查询时会话答走主口(留待有真需求再细化)。
 */
#include "mdns_responder.h"
#include "mdns_wire.h"
#include "net_info.h"
#include "netcore.h"
#include "cfg.h"
#include "storage.h"
#include "dg_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[MDNS]";

#define DEFAULT_HOST     "doorguard"
#define SERVICE_TYPE     "_http._tcp.local"
#define TTL_HOST         120      /* A 记录:IP 不常变(RFC 建议 ≤120) */
#define TTL_SERVICE      4500     /* PTR/SRV/TXT:服务发现用长 TTL */
#define TTL_LEGACY       10       /* 单播应答:压到 10s,避免进组播缓存 */

#define PROBE_COUNT      3
#define PROBE_GAP_MS     250
#define ANNOUNCE_COUNT   2
#define ANNOUNCE_GAP_MS  1000
#define IP_CHECK_MS      5000
#define TICK_MS          200      /* 状态机基准节拍(原 recvmsg 超时轮) */

typedef enum { ST_IDLE = 0, ST_PROBING, ST_ANNOUNCING, ST_READY } mdns_state_t;

/* 命中了哪些问题 → 决定应答里放哪些记录 */
typedef struct {
    bool host_a;
    bool service_ptr;
    bool srv;
    bool txt;
} mdns_hits_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_running = false;

static char s_base[40] = DEFAULT_HOST;         /* 配置的基名(重名时加后缀) */
static char s_host[64] = DEFAULT_HOST;         /* 实际通告名,不含 .local */
static uint16_t s_port = 8080;
static mdns_state_t s_state = ST_IDLE;
static int s_step = 0;                         /* 探测/通告已发次数 */
static uint8_t s_ip[4];                        /* 当前通告地址 */

/* 以下为 loop 线程私有(netcore 契约):不与业务线程共享,无需加锁 */
static struct mg_connection *s_udp = NULL;     /* UDP 5353 监听连接 */
static int64_t s_next_action = 0;              /* 状态机下一拍 */
static bool s_timer_added = false;             /* 节拍定时器只注册一次 */

/* ---- 工具 ---- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 取名字/端口快照(锁内拷贝):重名改名发生在探测期,与 READY 应答并发,
 * 不锁就读 s_host 会读到半截字符串。
 * **调用方不得已持有 s_mtx**(非递归锁,持锁再调即自死锁)。 */
static void snapshot(char *host, size_t hcap, char *inst, size_t icap,
                     uint16_t *port)
{
    pthread_mutex_lock(&s_mtx);
    if (host && hcap)
        snprintf(host, hcap, "%s.local", s_host);
    if (inst && icap)
        snprintf(inst, icap, "%s.%s", s_host, SERVICE_TYPE);
    if (port)
        *port = s_port;
    pthread_mutex_unlock(&s_mtx);
}

static void multicast_dst(struct sockaddr_in *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MULTICAST, &dst->sin_addr);
}

static int udp_fd(void)
{
    return s_udp ? (int)(intptr_t)s_udp->fd : -1;
}

/* 组播出口绑定到主网口(见文件头"接口绑定"说明) */
static void bind_iface(int sock, int ifindex)
{
    if (ifindex <= 0)
        return;
    struct ip_mreqn mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_ifindex = ifindex;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq));
}

static int out_ifindex(void)
{
    char ifname[32] = "";
    net_info_primary_ifname(ifname, sizeof(ifname));
    return ifname[0] ? (int)if_nametoindex(ifname) : 0;
}

/* ---- 应答构造 ---- */

static void send_response(int fd, const struct sockaddr_in *from, int ifindex,
                          uint16_t id, bool legacy, const mdns_hits_t *hits,
                          const mdns_question_t *qs, int nq)
{
    uint8_t buf[1024];
    mdns_msg_t m;
    char host[96], inst[96];
    uint16_t port;
    static const char *const txt[] = { "txtvers=1", "path=/" };

    snapshot(host, sizeof(host), inst, sizeof(inst), &port);
    uint32_t ttl_host = legacy ? TTL_LEGACY : TTL_HOST;
    uint32_t ttl_svc = legacy ? TTL_LEGACY : TTL_SERVICE;
    bool flush = !legacy;                      /* 单播应答不带 cache-flush */

    mdns_msg_begin(&m, buf, sizeof(buf), id,
                   (uint16_t)(MDNS_FLAG_RESPONSE | MDNS_FLAG_AA));

    /* legacy resolver 要求应答里原样回带它的问题段(RFC 6762 §6.7),
     * 否则老实现认不出这是答案 */
    if (legacy && qs) {
        for (int i = 0; i < nq; i++)
            mdns_msg_put_question(&m, qs[i].name, qs[i].type,
                                  (uint16_t)(qs[i].klass |
                                             (qs[i].unicast ? MDNS_CLASS_TOP : 0)));
    }

    if (hits->host_a)
        mdns_msg_put_a(&m, host, ttl_host, flush, s_ip);
    if (hits->service_ptr)
        mdns_msg_put_ptr(&m, SERVICE_TYPE, ttl_svc, inst);
    if (hits->srv)
        mdns_msg_put_srv(&m, inst, ttl_svc, flush, 0, 0, port, host);
    if (hits->txt)
        mdns_msg_put_txt(&m, inst, ttl_svc, flush, txt, 2);
    if (hits->srv)                             /* SRV 目标地址附在 additional */
        mdns_msg_put_additional_a(&m, host, ttl_host, s_ip);

    if (mdns_msg_end(&m) != DG_OK) {
        DG_LOGW(TAG, "应答报文构造失败(缓冲不足)");
        return;
    }

    struct sockaddr_in dst;
    if (legacy) {
        dst = *from;                           /* 单播回给提问者 */
    } else {
        multicast_dst(&dst);                   /* 组播:全局通告 */
        bind_iface(fd, ifindex);               /* 从主网口发出去 */
    }
    if (sendto(fd, buf, m.len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
        DG_LOGW(TAG, "应答发送失败: %s", strerror(errno));
}

/* ---- 查询处理 ---- */

static void handle_query(const uint8_t *pkt, size_t len,
                         const struct sockaddr_in *from, int ifindex)
{
    uint16_t id = 0, flags = 0;
    mdns_question_t qs[8];
    int nq = mdns_parse_query(pkt, len, &id, &flags, qs, 8);
    if (nq < 0 || nq == 0)
        return;                                /* 坏报文/无问题:丢掉,不崩 */
    if ((flags & MDNS_FLAG_RESPONSE) != 0)
        return;                                /* QR=1:别人的应答,不是查询 */

    char host[96], inst[96];
    uint16_t port;
    snapshot(host, sizeof(host), inst, sizeof(inst), &port);

    mdns_hits_t hits = { false, false, false, false };
    bool unicast = (ntohs(from->sin_port) != MDNS_PORT);
    for (int i = 0; i < nq && i < 8; i++) {
        if (qs[i].klass != MDNS_CLASS_IN)
            continue;
        if (qs[i].unicast)
            unicast = true;
        bool any = (qs[i].type == MDNS_TYPE_ANY);
        if (mdns_name_equal(qs[i].name, host)) {
            if (any || qs[i].type == MDNS_TYPE_A)
                hits.host_a = true;
        } else if (mdns_name_equal(qs[i].name, SERVICE_TYPE)) {
            if (any || qs[i].type == MDNS_TYPE_PTR)
                hits.service_ptr = true;
        } else if (mdns_name_equal(qs[i].name, inst)) {
            if (any || qs[i].type == MDNS_TYPE_SRV)
                hits.srv = true;
            if (any || qs[i].type == MDNS_TYPE_TXT)
                hits.txt = true;
        }
    }
    if (!hits.host_a && !hits.service_ptr && !hits.srv && !hits.txt)
        return;

    DG_LOGI(TAG, "%s查询 →%s应答(addr=%d svc=%d srv=%d txt=%d)",
            mdns_name_equal(qs[0].name, host) ? "主机名" : "服务",
            unicast ? "单播" : "组播", hits.host_a, hits.service_ptr, hits.srv,
            hits.txt);
    send_response(udp_fd(), from, ifindex, unicast ? id : 0, unicast, &hits,
                  qs, nq);
}

/* ---- 探测期冲突检测:有人用同名但 IP 不同 ---- */

typedef struct {
    char    host[96];
    uint8_t our_ip[4];
    bool    conflict;
} probe_ctx_t;

static int probe_rr_cb(const mdns_rr_t *rr, void *ud)
{
    probe_ctx_t *c = (probe_ctx_t *)ud;
    if (rr->type != MDNS_TYPE_A || rr->rdlen != 4 || rr->ttl == 0)
        return 0;                              /* 非 A / goodbye:不算冲突 */
    if (!mdns_name_equal(rr->name, c->host))
        return 0;
    if (memcmp(rr->rdata, c->our_ip, 4) == 0)
        return 0;                              /* 自己的组播回环 */
    c->conflict = true;
    return 1;
}

static void check_conflict(const uint8_t *pkt, size_t len)
{
    if ((pkt[2] & 0x80) == 0)                  /* 只看应答 */
        return;

    pthread_mutex_lock(&s_mtx);
    bool have_ip = (s_ip[0] | s_ip[1] | s_ip[2] | s_ip[3]) != 0;
    pthread_mutex_unlock(&s_mtx);
    if (!have_ip)
        return;                                /* 自己还没地址:谈不上冲突 */

    probe_ctx_t c;
    memset(&c, 0, sizeof(c));
    snapshot(c.host, sizeof(c.host), NULL, 0, NULL);
    pthread_mutex_lock(&s_mtx);
    memcpy(c.our_ip, s_ip, 4);
    pthread_mutex_unlock(&s_mtx);

    if (mdns_walk_rrs(pkt, len, probe_rr_cb, &c) <= 0 && !c.conflict)
        return;

    /* 改名重探:后缀递增(doorguard → -2 → -3…),并写回 device_config——
     * 名字必须稳定,否则每次开机都给局域网一个新名字,缓存全废 */
    pthread_mutex_lock(&s_mtx);
    int n = 1;
    const char *dash = strrchr(s_host, '-');
    if (dash && dash[1])
        n = atoi(dash + 1);
    if (n < 1)
        n = 1;
    n++;
    if (n > 9 || strlen(s_base) + 3 >= sizeof(s_host)) {
        DG_LOGE(TAG, "重名后缀已到上限,保持 %s.local(局域网可能有同名设备)", s_host);
        s_state = ST_READY;
        pthread_mutex_unlock(&s_mtx);
        return;
    }
    snprintf(s_host, sizeof(s_host), "%s-%d", s_base, n);
    DG_LOGW(TAG, "检测到重名,改用 %s.local 重新探测", s_host);
    s_state = ST_PROBING;
    s_step = 0;
    char persisted[64];
    snprintf(persisted, sizeof(persisted), "%s", s_host);
    pthread_mutex_unlock(&s_mtx);
    db_config_set("mdns_host", persisted);
}

/* ---- 探测 / 通告 / goodbye ---- */

static void send_probe(int ifindex)
{
    uint8_t buf[512];
    mdns_msg_t m;
    char host[96], inst[96];
    struct sockaddr_in dst;
    int fd = udp_fd();
    if (fd < 0)
        return;

    snapshot(host, sizeof(host), inst, sizeof(inst), NULL);
    /* 探测用 ID=0(组播),问题段问 ANY:既查 A 也查服务名占用 */
    mdns_msg_begin(&m, buf, sizeof(buf), 0, 0);
    mdns_msg_put_question(&m, host, MDNS_TYPE_ANY, MDNS_CLASS_IN);
    mdns_msg_put_question(&m, inst, MDNS_TYPE_ANY, MDNS_CLASS_IN);
    if (mdns_msg_end(&m) != DG_OK)
        return;

    multicast_dst(&dst);
    bind_iface(fd, ifindex);
    sendto(fd, buf, m.len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void send_announce(int ifindex)
{
    mdns_hits_t all = { true, true, true, true };
    struct sockaddr_in dst;
    int fd = udp_fd();
    if (fd < 0)
        return;
    multicast_dst(&dst);
    bind_iface(fd, ifindex);
    send_response(fd, &dst, ifindex, 0, false, &all, NULL, 0);
}

/* goodbye:TTL=0 的同名记录,让局域网立刻删掉我们(不留幽灵条目) */
static void send_goodbye(void)
{
    uint8_t buf[1024];
    mdns_msg_t m;
    char host[96], inst[96];
    uint16_t port;
    static const char *const txt[] = { "txtvers=1", "path=/" };
    struct sockaddr_in dst;
    int fd = udp_fd();

    if (fd < 0)
        return;
    snapshot(host, sizeof(host), inst, sizeof(inst), &port);
    mdns_msg_begin(&m, buf, sizeof(buf), 0,
                   (uint16_t)(MDNS_FLAG_RESPONSE | MDNS_FLAG_AA));
    mdns_msg_put_a(&m, host, 0, true, s_ip);
    mdns_msg_put_ptr(&m, SERVICE_TYPE, 0, inst);
    mdns_msg_put_srv(&m, inst, 0, true, 0, 0, port, host);
    mdns_msg_put_txt(&m, inst, 0, true, txt, 2);
    if (mdns_msg_end(&m) != DG_OK)
        return;
    multicast_dst(&dst);
    sendto(fd, buf, m.len, 0, (struct sockaddr *)&dst, sizeof(dst));
    DG_LOGI(TAG, "已发 goodbye");
}

/* ---- IP 跟踪 ---- */

static void refresh_ip(void)
{
    char ip[64];
    if (net_info_primary_ipv4(ip, sizeof(ip)) != DG_OK)
        return;
    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1)
        return;
    uint8_t newip[4];
    memcpy(newip, &a.s_addr, 4);
    pthread_mutex_lock(&s_mtx);
    bool changed = memcmp(newip, s_ip, 4) != 0;
    memcpy(s_ip, newip, 4);
    pthread_mutex_unlock(&s_mtx);
    if (changed)
        DG_LOGI(TAG, "通告地址 %s", ip);
}

/* ---- 统一事件循环上的收包与节拍(loop 线程) ---- */

static void mdns_udp_cb(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_READ) {
        if (c->recv.len == 0)
            return;
        pthread_mutex_lock(&s_mtx);
        mdns_state_t st = s_state;
        pthread_mutex_unlock(&s_mtx);

        struct sockaddr_in from;
        memset(&from, 0, sizeof(from));
        from.sin_family = AF_INET;
        memcpy(&from.sin_addr, c->rem.addr.ip, 4);
        from.sin_port = c->rem.port;

        if (st == ST_PROBING)
            check_conflict(c->recv.buf, c->recv.len);
        else if (st == ST_READY)
            handle_query(c->recv.buf, c->recv.len, &from, 0);
        /* 探测/通告期不抢答:名字还没确认属于我们 */
        mg_iobuf_del(&c->recv, 0, c->recv.len);
    } else if (ev == MG_EV_CLOSE) {
        s_udp = NULL;
    }
    (void)ev_data;
}

static void mdns_tick(void *arg)
{
    (void)arg;
    if (!s_udp)
        return;
    int64_t t = now_ms();
    if (t < s_next_action)
        return;
    s_next_action = t + TICK_MS;
    int ifindex = out_ifindex();

    pthread_mutex_lock(&s_mtx);
    mdns_state_t st = s_state;
    pthread_mutex_unlock(&s_mtx);

    switch (st) {
    case ST_PROBING:
        send_probe(ifindex);
        pthread_mutex_lock(&s_mtx);
        if (++s_step >= PROBE_COUNT) {
            s_state = ST_ANNOUNCING;
            s_step = 0;
        }
        pthread_mutex_unlock(&s_mtx);
        s_next_action = t + PROBE_GAP_MS;
        break;
    case ST_ANNOUNCING: {
        send_announce(ifindex);
        pthread_mutex_lock(&s_mtx);
        if (++s_step >= ANNOUNCE_COUNT) {
            s_state = ST_READY;
            s_step = 0;
            /* 注意:此处已持锁,只能直接读 s_host——snapshot() 会再取同一把
             * 非递归互斥锁,曾经在此自死锁并连带卡死 web 线程 */
            DG_LOGI(TAG, "%s.local 通告完成(_http._tcp 服务已公告)", s_host);
        }
        pthread_mutex_unlock(&s_mtx);
        s_next_action = t + ANNOUNCE_GAP_MS;
        break;
    }
    case ST_READY: {
        /* IP 变了(拔插网线/DHCP 换租约)必须重通告,否则局域网缓存里
         * 还是旧地址——这是"按名字访问"最常见的失灵原因 */
        char before[64] = "";
        pthread_mutex_lock(&s_mtx);
        snprintf(before, sizeof(before), "%u.%u.%u.%u", s_ip[0], s_ip[1],
                 s_ip[2], s_ip[3]);
        pthread_mutex_unlock(&s_mtx);
        refresh_ip();
        char after[64] = "";
        pthread_mutex_lock(&s_mtx);
        snprintf(after, sizeof(after), "%u.%u.%u.%u", s_ip[0], s_ip[1],
                 s_ip[2], s_ip[3]);
        pthread_mutex_unlock(&s_mtx);
        if (strcmp(before, after) != 0) {
            DG_LOGI(TAG, "地址变化,重新通告");
            send_announce(ifindex);
        }
        s_next_action = t + IP_CHECK_MS;
        break;
    }
    default:
        break;
    }
}

/* ---- 启动/停止(netcore_post 闭包,loop 线程执行) ---- */

static void mdns_udp_cb(struct mg_connection *c, int ev, void *ev_data);
static int join_all_interfaces(int sock);

static void mdns_setup(void *arg)
{
    (void)arg;
    if (!s_timer_added) {
        mg_timer_add(netcore_mgr(), TICK_MS, MG_TIMER_REPEAT, mdns_tick, NULL);
        s_timer_added = true;
    }
    s_udp = mg_listen(netcore_mgr(), "udp://0.0.0.0:5353", mdns_udp_cb, NULL);
    if (!s_udp) {
        pthread_mutex_lock(&s_mtx);
        s_running = false;
        pthread_mutex_unlock(&s_mtx);
        DG_LOGW(TAG, "5353 监听失败,mDNS 不可用");
        return;
    }

    int fd = udp_fd();
    unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    int on = 1;
    /* IP_PKTINFO 备用:mongoose 不透出入接口,单网口场景不需要 */
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on));
    if (join_all_interfaces(fd) == 0)
        DG_LOGW(TAG, "没有一个网口入组成功:组播可能收不到");

    refresh_ip();
    pthread_mutex_lock(&s_mtx);
    s_state = ST_PROBING;
    s_step = 0;
    pthread_mutex_unlock(&s_mtx);
    s_next_action = now_ms();

    char host[96];
    uint16_t port;
    snapshot(host, sizeof(host), NULL, 0, &port);
    DG_LOGI(TAG, "启动:%s(%u 端口)探测中", host, (unsigned)port);
}

static void mdns_teardown(void *arg)
{
    (void)arg;
    send_goodbye();                            /* 先道别,再收摊 */
    if (s_udp) {
        s_udp->is_closing = 1;                 /* 延迟关闭(同 web_teardown 注释) */
        s_udp = NULL;
    }
    pthread_mutex_lock(&s_mtx);
    s_state = ST_IDLE;
    s_step = 0;
    pthread_mutex_unlock(&s_mtx);
}

/* 逐接口入组:只做 INADDR_ANY 入组时,组播包在部分内核/驱动组合下收不到
 * (实测 eth 口必须按接口入组) */
static int join_all_interfaces(int sock)
{
    int joined = 0;
    for (unsigned i = 1; i < 32; i++) {
        char name[IF_NAMESIZE];
        if (if_indextoname(i, name) == NULL)
            continue;
        struct ip_mreqn mreq;
        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, MDNS_MULTICAST, &mreq.imr_multiaddr);
        mreq.imr_ifindex = (int)i;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                       sizeof(mreq)) == 0)
            joined++;
    }
    return joined;
}

int mdns_start(void)
{
    if (mdns_running())
        return DG_OK;
    if (!netcore_running()) {
        DG_LOGW(TAG, "netcore 未运行,mDNS 不启动");
        return DG_ERR_IO;
    }

    /* 主机名限 39 字符:留出 "-9" 后缀空间,重名改名时拼装不会截断 */
    char hbuf[40] = "";
    if (db_config_get("mdns_host", hbuf, sizeof(hbuf)) != DG_OK || !hbuf[0])
        snprintf(hbuf, sizeof(hbuf), "%s", DEFAULT_HOST);

    pthread_mutex_lock(&s_mtx);
    snprintf(s_base, sizeof(s_base), "%s", hbuf);
    snprintf(s_host, sizeof(s_host), "%s", hbuf);
    const dg_cfg_t *cfg = cfg_get();
    s_port = (uint16_t)((cfg && cfg->web_port > 0) ? cfg->web_port : 8080);
    s_running = true;
    pthread_mutex_unlock(&s_mtx);

    netcore_post(mdns_setup, NULL);
    return DG_OK;
}

void mdns_stop(void)
{
    if (!mdns_running())
        return;
    pthread_mutex_lock(&s_mtx);
    s_running = false;
    pthread_mutex_unlock(&s_mtx);
    /* netcore 已停则闭包被静默丢弃(连接由 mg_mgr_free 兜底关闭) */
    netcore_post(mdns_teardown, NULL);
}

bool mdns_running(void)
{
    pthread_mutex_lock(&s_mtx);
    bool r = s_running;
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int mdns_hostname(char *out, size_t cap)
{
    if (!out || cap == 0)
        return DG_ERR_PARAM;
    pthread_mutex_lock(&s_mtx);
    snprintf(out, cap, "%s", s_host);
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

int mdns_url(char *out, size_t cap)
{
    if (!out || cap == 0)
        return DG_ERR_PARAM;
    /* 以"名字形式"为主、IP 作括号补注:名字才是我们做 mDNS 的目的
     * (换 DHCP 租约/换网段都不用改),IP 只在名字解析不了的旧设备上兜底 */
    char host[96];
    snapshot(host, sizeof(host), NULL, 0, NULL);
    uint16_t port = s_port;
    char ip[64];
    if (net_info_primary_ipv4(ip, sizeof(ip)) == DG_OK)
        return snprintf(out, cap, "http://%s:%u (%s)", host, (unsigned)port, ip) > 0
                   ? DG_OK : DG_ERR_NO_MEMORY;
    return snprintf(out, cap, "http://%s:%u", host, (unsigned)port) > 0
               ? DG_OK : DG_ERR_NO_MEMORY;
}

int mdns_announce(void)
{
    if (!mdns_running())
        return DG_ERR_NOT_INIT;
    /* 端口/名字可能刚被菜单/上位机改过:先同步配置,再重走一遍通告 */
    pthread_mutex_lock(&s_mtx);
    const dg_cfg_t *cfg = cfg_get();
    if (cfg && cfg->web_port > 0)
        s_port = (uint16_t)cfg->web_port;
    s_state = ST_ANNOUNCING;
    s_step = 0;
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}
