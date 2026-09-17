/*
 * task_worker.h — tasker 执行线程(内部头)
 *
 * 移植自模板 ovs/components/core/tasker/task_worker.h;差异:logger 换 dg_log。
 */
#ifndef TASK_WORKER
#define TASK_WORKER

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

#include "task_manager.h"
#include "tasker_port.h"
#include "dg_log.h"

extern const char *TASK_WORKER_TAG;
extern atomic_int worker_init_flag;

struct task_worker_ctx {
    struct task_worker *little_worker;
    struct task_worker *middle_worker;
    struct task_worker *lots_worker;
    struct task_worker *s_dispatcher;
    struct task_worker *s_sched_table;
};

extern struct task_worker_ctx s_task_worker_ctx;

struct task_worker {
    atomic_int stop;       /* 主线程置位、工作线程轮询,跨线程 → 原子 */
    int timeout_flag;
    tasker_timer_t timeout_timer;
    int pt_created;                       /* pthread_create 成功标记(安全 join) */
    pthread_t pt;
    pthread_mutex_t mtx;
    pthread_cond_t cond;

    struct task_manager *worker_queue;    /* 本线程的任务表 */
};

int worker_init(void);
void worker_delete(struct task_worker *worker);

int worker_little_init(void);
int worker_middle_init(void);
int worker_lots_init(void);
int worker_dispatcher_init(void);
int worker_sched_init(void);

/* 各执行线程入口(pthread_create 期望 void* (*)(void*)) */
void *worker_little_handler(void *arg);
void *worker_middle_handler(void *arg);
void *worker_lots_handler(void *arg);
void *worker_dispatcher_handler(void *arg);
void *worker_sched_handler(void *arg);
void worker_do_handler(struct task_worker *worker);

int worker_sched_enqueue(struct task_node *node);
int worker_task_enqueue(struct task_worker *worker, struct task_node *node);
void worker_task_done(struct task_worker *worker, struct task_node *node);
void worker_task_cancel(struct task_worker *worker, struct task_node *node);

#endif /* TASK_WORKER */
