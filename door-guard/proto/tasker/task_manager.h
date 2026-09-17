/*
 * task_manager.h — 任务节点与调度表(tasker 内部)
 *
 * 移植自模板 ovs/components/core/tasker/task_manager.h;与模板差异:
 * logger.h 换为 proto/dg_log.h,mem.h 换为标准 malloc/free。
 */
#ifndef TASK_MANAGER
#define TASK_MANAGER

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "dg_log.h"
#include "tasker_port.h"

/* 分级队列容量:模板稳定值;已满时通过 TASK_QUEUE_FULL 上报并由调度器
 * 优先级提升重排,不静默丢弃 */
#define LITTLE_TASK_QUEUE_SIZE 8
#define MIDDLE_TASK_QUEUE_SIZE 8
#define LOTS_TASK_QUEUE_SIZE 8
#define DISPATCHER_TASK_QUEUE_SIZE 64
#define SCHED_TASK_QUEUE_SIZE 64

/* pthread 线程栈提示值(Linux 按需补页) */
#define LITTLE_TASK_STACK_SIZE (64 * 1024)
#define MIDDLE_TASK_STACK_SIZE (64 * 1024)
#define LOTS_TASK_STACK_SIZE (128 * 1024)
#define DISPATCHER_TASK_QUEUE_STACK_SIZE (64 * 1024)
#define SCHED_TASK_QUEUE_STACK_SIZE (64 * 1024)

#define LITTLE_TASK_DEFAULT_TIMEOUT 50  /* ms */
#define MIDDLE_TASK_DEFAULT_TIMEOUT 1000 /* ms */
#define LOTS_TASK_DEFAULT_TIMEOUT 10000 /* ms */

extern const char *TASK_MANAGER_TAG;

enum task_t {
    TASK_OK = 0,
    TASK_MEM_ERR = -1,
    TASK_TIMEOUT_ERR = -2,
    TASK_PARA_ERR = -3,
    TASK_FUNC_ERR = -4,
    TASK_INNER_ERR = -5,
    TASK_QUEUE_FULL = -6,
    TASK_STOP = -7,
};

/* priority 维度已废弃(模板 5.2 约定),仅为兼容保留枚举 */
enum task_priority {
    first = 1,
    middle,
    last,
};

/* 时间成本分级:决定进哪个执行线程 */
enum task_time_cost_level {
    level_little = 1,
    level_middle,
    level_lots,
};

typedef enum task_t (*task_fn)(void *ctx);

struct task_node {
    /* cancel/done/dispatched 跨线程读写(模板 5.2 修复2: 原子化) */
    atomic_int done;
    atomic_int cancel;
    int timeout;
    int is_timeout;        /* 仅执行线程写、无跨线程读,保持普通 int */
    int period;
    atomic_int run_cnt;    /* sched 写、执行线程读,跨互斥域 → 原子 */
    atomic_int pri;        /* dispatcher/sched/worker 三线程读改写 → 原子 */
    atomic_int level;      /* 同上(超时升级/队列满升级) */
    task_fn fn;            /* 入队前写定,入队后只读(发布经互斥锁同步) */
    uint64_t inject_time;  /* 仅 sched 线程在自家锁内读写 */
    char name[32];
    void *ctx;
    atomic_int dispatched; /* 1=节点被分发/执行线程借用中 */
};

struct task_manager {
    unsigned int size;
    struct task_node **queue;
};

/* 实时计算队列状态(无陈旧标志) */
bool task_manager_is_empty(const struct task_manager *mgr);
bool task_manager_is_full(const struct task_manager *mgr);

struct task_manager *task_manager_init(const unsigned int size);

struct task_node *task_node_pool_alloc(void);
void task_node_pool_free(struct task_node *node);

/* 调用方节点就地初始化(无堆分配) */
void task_node_init(struct task_node *node,
                    const int timeout,
                    const uint64_t inject_time,
                    const int period,
                    const int run_cnt,
                    const enum task_priority pri,
                    const enum task_time_cost_level level,
                    const char *name,
                    task_fn fn, void *ctx);

void task_node_pri_up(struct task_node *node);
void task_node_leve_up(struct task_node *node);

struct task_node *find_task_node_by_name(struct task_manager *worker_queue, const char *name);

void task_manager_pri_sort(struct task_manager *worker_queue);

void task_done(struct task_node *node);
int task_is_done(struct task_node *node);
void task_cancel(struct task_node *node);
int task_is_cancel(struct task_node *node);

#endif /* TASK_MANAGER */
