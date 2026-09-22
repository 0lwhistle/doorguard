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

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

int netcore_start(void)
{
    if (atomic_load(&s_running))
        return DG_OK;

    memset(&s_mgr, 0, sizeof(s_mgr));
    mg_mgr_init(&s_mgr);
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
