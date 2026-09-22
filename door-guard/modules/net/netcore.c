/*
 * netcore.c — 统一网络事件循环实现
 *
 * 单线程事件循环:mg_mgr_poll 驻留 loop 线程;跨线程只经 netcore_post
 * 投递闭包(定长环形队列,满丢最旧并计数——与 web WS 推送队列同一策略:
 * 实时事件宁可丢也不能堵住总线线程)。排水用 10ms 周期定时器:验证类
 * 事件(WS 推送/NTP 触发)对 10ms 级延迟不敏感,不值得为瞬时唤醒引入
 * 跨线程唤醒管道的额外复杂度;loop 的 poll 超时 50ms 保证停服在几十毫秒
 * 内完成 join。
 *
 * 心跳:每轮 poll 刷新一次,看门狗经 web_server_heartbeat_ms 透传消费,
 * 语义与原"推送线程心跳"一致。
 */
#include "netcore.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "dg_log.h"

static const char *TAG = "[NETCORE]";

#define NETCORE_POST_MAX   32         /* 闭包队列容量(丢最旧) */
#define NETCORE_POLL_MS    50         /* poll 超时 = 停服延迟上界 */
#define NETCORE_DRAIN_MS   10         /* post 队列排水周期 */

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} post_t;

static struct mg_mgr s_mgr;
static pthread_t s_tid;
static bool s_tid_valid = false;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static post_t s_q[NETCORE_POST_MAX];
static int s_head = 0, s_tail = 0;
static int s_dropped = 0;
/* 跨线程标志/心跳一律原子量:TSAN 下零竞态,语义与朴素读写一致 */
static atomic_bool s_running = false;
static atomic_llong s_hb_ms = 0;

/* 项目心跳约定 = CLOCK_REALTIME 纪元毫秒:main 看门狗的 now_ms 即
 * REALTIME(旧 web 推送线程心跳同此)。换 MONOTONIC 会与看门狗相差
 * 整个纪元基数,心跳必然被判"超龄"(板上实测每次启动误报重启) */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* loop 线程内:排空闭包队列。轮数上界防"闭包再投闭包"饿死 loop;
 * 剩余的下一轮定时器接着排 */
static void drain_posts(void)
{
    for (int i = 0; i < NETCORE_POST_MAX; i++) {
        pthread_mutex_lock(&s_mtx);
        if (s_head == s_tail) {
            pthread_mutex_unlock(&s_mtx);
            return;
        }
        post_t p = s_q[s_tail];
        s_tail = (s_tail + 1) % NETCORE_POST_MAX;
        pthread_mutex_unlock(&s_mtx);
        p.fn(p.arg);
    }
}

static void drain_timer_fn(void *arg)
{
    (void)arg;
    drain_posts();
}

static void *loop_thread(void *arg)
{
    (void)arg;
    mg_timer_add(&s_mgr, NETCORE_DRAIN_MS, MG_TIMER_REPEAT, drain_timer_fn, NULL);
    while (atomic_load(&s_running)) {
        atomic_store(&s_hb_ms, now_ms());
        mg_mgr_poll(&s_mgr, NETCORE_POLL_MS);
    }
    return NULL;
}

/* mongoose 内置 DNS 客户端默认查 8.8.8.8(3s 超时)——家用路由/内网常不可达
 * (板上实测:SNTP 解析 ntp.aliyun.com 超时,而本机 nslookup 走路由器秒回)。
 * 从 /etc/resolv.conf 取首个 nameserver 覆盖默认值;容错 dhcpcd 风格的
 * 行尾注释("nameserver 192.168.2.1 # eth0")。仅启动时读一次。 */
static void load_dns_server(void)
{
    static char dns_url[64];                 /* 被 mgr 引用,须与 mgr 同生命周期 */

    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) {
        DG_LOGW(TAG, "读 /etc/resolv.conf 失败,DNS 用默认 8.8.8.8");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';                    /* 行尾注释 */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "nameserver", 10) != 0)
            continue;
        char *ip = p + 10;
        while (*ip == ' ' || *ip == '\t')
            ip++;
        char *end = ip;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r')
            end++;
        *end = '\0';
        if (!ip[0])
            continue;
        snprintf(dns_url, sizeof(dns_url), "udp://%s:53", ip);
        s_mgr.dns4.url = dns_url;
        DG_LOGI(TAG, "DNS 服务器: %s", ip);
        break;
    }
    fclose(f);
}

int netcore_start(void)
{
    if (atomic_load(&s_running))
        return DG_OK;

    memset(&s_mgr, 0, sizeof(s_mgr));
    mg_mgr_init(&s_mgr);
    load_dns_server();
    /* 默认 3s:家用路由对未缓存域名的递归解析常超(板上实测 ntp.aliyun.com
     * 首查 3s+);10s 与 SNTP 超时同量级,失败路径不受影响 */
    s_mgr.dnstimeout = 10000;
    s_head = s_tail = s_dropped = 0;
    atomic_store(&s_hb_ms, 0);

    atomic_store(&s_running, true);
    if (pthread_create(&s_tid, NULL, loop_thread, NULL) != 0) {
        atomic_store(&s_running, false);
        mg_mgr_free(&s_mgr);
        DG_LOGE(TAG, "loop 线程创建失败,网络层不可用");
        return DG_ERR_IO;
    }
    s_tid_valid = true;
    DG_LOGI(TAG, "统一网络事件循环就绪");
    return DG_OK;
}

void netcore_stop(void)
{
    if (!atomic_load(&s_running))
        return;

    pthread_mutex_lock(&s_mtx);
    atomic_store(&s_running, false);        /* loop 线程 ≤POLL_MS 内退出 */
    pthread_mutex_unlock(&s_mtx);
    if (s_tid_valid) {
        pthread_join(s_tid, NULL);
        s_tid_valid = false;
    }
    mg_mgr_free(&s_mgr);                     /* 关闭所有残留连接 */
    atomic_store(&s_hb_ms, 0);
    DG_LOGI(TAG, "统一网络事件循环已停止(闭包丢弃累计 %d)", s_dropped);
}

bool netcore_running(void)
{
    return atomic_load(&s_running);
}

int64_t netcore_heartbeat_ms(void)
{
    return atomic_load(&s_hb_ms);
}

void netcore_post(void (*fn)(void *arg), void *arg)
{
    if (!fn)
        return;
    pthread_mutex_lock(&s_mtx);
    if (!atomic_load(&s_running)) {
        pthread_mutex_unlock(&s_mtx);
        return;                              /* 停服竞态:静默丢弃 */
    }
    int next = (s_head + 1) % NETCORE_POST_MAX;
    if (next == s_tail) {                    /* 满:丢最旧 */
        s_tail = (s_tail + 1) % NETCORE_POST_MAX;
        s_dropped++;
    }
    s_q[s_head].fn = fn;
    s_q[s_head].arg = arg;
    s_head = next;
    pthread_mutex_unlock(&s_mtx);
}

struct mg_mgr *netcore_mgr(void)
{
    return &s_mgr;                           /* 仅限 loop 线程使用(见头文件) */
}
