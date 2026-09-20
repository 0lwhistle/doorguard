/*
 * event_bus_port.c — pthread 移植实现(出处与改动点见本目录 README.md)
 *
 * 基础为模板 PC 分支;主要改动:
 * 1. bus_lock_take 由 trylock+nanosleep 轮询改为 pthread_mutex_timedlock
 *    (语义相同,不烧 CPU);
 * 2. 任务增加 stop 标志 + join:模板 PC 分支 deinit 时任务永不退出且句柄
 *    泄漏,door-guard 纪律要求"中途退出有清理路径",故补齐。
 */
#include "event_bus_port.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- 队列:有界环形缓冲,mutex + not_empty 条件变量 ---------- */

struct bus_queue {
    uint8_t *buf;
    int depth;
    int item_size;
    int head;
    int tail;
    int count;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
};

bus_queue_t *bus_queue_create(int depth, int item_size)
{
    if (depth <= 0 || item_size <= 0)
        return NULL;

    bus_queue_t *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    q->buf = calloc((size_t)depth, (size_t)item_size);
    if (!q->buf) {
        free(q);
        return NULL;
    }
    q->depth = depth;
    q->item_size = item_size;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

/* 绝对超时 = CLOCK_REALTIME 当前值 + timeout_ms(cond 默认时钟) */
static void abs_deadline(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

bool bus_queue_send(bus_queue_t *q, void *item)
{
    if (!q)
        return false;
    bool ok = false;
    pthread_mutex_lock(&q->mtx);
    if (q->count < q->depth) {
        memcpy(q->buf + (size_t)q->head * q->item_size, item, (size_t)q->item_size);
        q->head = (q->head + 1) % q->depth;
        q->count++;
        ok = true;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mtx);
    return ok;
}

bool bus_queue_recv(bus_queue_t *q, void *item, int timeout_ms)
{
    if (!q)
        return false;
    struct timespec ts;
    abs_deadline(&ts, timeout_ms);

    bool ok = false;
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        if (pthread_cond_timedwait(&q->not_empty, &q->mtx, &ts) == ETIMEDOUT)
            break;
    }
    if (q->count > 0) {
        memcpy(item, q->buf + (size_t)q->tail * q->item_size, (size_t)q->item_size);
        q->tail = (q->tail + 1) % q->depth;
        q->count--;
        ok = true;
    }
    pthread_mutex_unlock(&q->mtx);
    return ok;
}

int bus_queue_count(bus_queue_t *q)
{
    if (!q)
        return 0;
    pthread_mutex_lock(&q->mtx);
    int n = q->count;
    pthread_mutex_unlock(&q->mtx);
    return n;
}

void bus_queue_destroy(bus_queue_t *q)
{
    if (!q)
        return;
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
    free(q);
}

/* ---------- 互斥锁 ---------- */

struct bus_lock { pthread_mutex_t mtx; };

bus_lock_t *bus_lock_create(void)
{
    bus_lock_t *l = malloc(sizeof(*l));
    if (!l)
        return NULL;
    pthread_mutex_init(&l->mtx, NULL);
    return l;
}

bool bus_lock_take(bus_lock_t *lock, int timeout_ms)
{
    if (!lock)
        return false;
    if (timeout_ms < 0)                     /* 永等 */
        return pthread_mutex_lock(&lock->mtx) == 0;

    struct timespec ts;
    abs_deadline(&ts, timeout_ms);
    return pthread_mutex_timedlock(&lock->mtx, &ts) == 0;
}

void bus_lock_give(bus_lock_t *lock)
{
    if (lock)
        pthread_mutex_unlock(&lock->mtx);
}

void bus_lock_destroy(bus_lock_t *lock)
{
    if (lock) {
        pthread_mutex_destroy(&lock->mtx);
        free(lock);
    }
}

/* ---------- 任务 ---------- */

struct bus_task {
    pthread_t tid;
    void (*fn)(void *);
    bool created;
};

static _Atomic(bool) s_task_stop = false;

static void *bus_task_trampoline(void *arg)
{
    bus_task_t *t = arg;
    t->fn(t);                   /* 句柄即任务参数,fn 内经 should_stop 感知停止 */
    return NULL;
}

bool bus_task_create(const char *name, int stack_size, int prio,
                     void (*fn)(void *), void **handle_out)
{
    (void)name;
    (void)prio;
    if (!fn || !handle_out)
        return false;

    bus_task_t *t = calloc(1, sizeof(*t));
    if (!t)
        return false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* stack_size 仅作提示:Linux 线程栈按需补页,超出即缺页扩展 */
    if (stack_size > 0)
        pthread_attr_setstacksize(&attr, (size_t)stack_size);

    t->fn = fn;
    s_task_stop = false;        /* 单例总线:新建任务前清残留停止位 */

    if (pthread_create(&t->tid, &attr, bus_task_trampoline, t) != 0) {
        pthread_attr_destroy(&attr);
        free(t);
        return false;
    }
    pthread_attr_destroy(&attr);
    t->created = true;
    *handle_out = t;
    return true;
}

bool bus_task_should_stop(void)
{
    return atomic_load(&s_task_stop);
}

void bus_task_delete(void *handle)
{
    bus_task_t *t = handle;
    if (!t)
        return;
    atomic_store(&s_task_stop, true);
    if (t->created) {
        /* 任务函数轮询 should_stop,最长 EVENT_BUS_POLL_INTERVAL_MS 退出 */
        pthread_join(t->tid, NULL);
    }
    free(t);
}

/* ---------- 时基:单调,不受系统改时影响 ---------- */

uint32_t bus_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
