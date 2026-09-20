/**
 * @file tasker.c
 * @brief Tasker API 实现
 * 
 * 提供 tasker 公共 API 的实现，内部使用 core/tasker 模块。
 */
/*
 * tasker.c — tasker 公共 API 实现
 *
 * 移植自模板 ovs/components/api/tasker_api/tasker.c;差异:logger 换 dg_log。
 */
#include "tasker.h"
#include "task_worker.h"
#include "tasker_port.h"
#include "dg_log.h"
#include <string.h>

/* 获取当前时间 (毫秒) */
static inline uint64_t tasker_get_time_ms(void) {
    return tasker_now_ms();
}

/* 自动初始化检查 */
static inline int tasker_auto_init(void) {
    if (!atomic_load(&worker_init_flag)) {
        int ret = worker_init();
        if (ret != 0) {
            DG_LOGE("[TASKER]", "tasker_init failed: %d", ret);
            return TASK_INNER_ERR;
        }
    }
    return TASK_OK;
}

/* 参数验证 */
static inline int tasker_validate_params(const int period, task_fn fn, const char* name) {
    if (period < 0 || !fn || !name || !strlen(name)) {
        DG_LOGW("[TASKER]", "tasker_task_init fail, invalid params");
        return TASK_PARA_ERR;
    }
    return TASK_OK;
}

int tasker_init(void) {
    return tasker_auto_init();
}

int tasker_enqueue(struct task_node* node) {
    if (!node) {
        DG_LOGW("[TASKER]", "tasker_enqueue fail, node is null");
        return TASK_PARA_ERR;
    }

    if (node->cancel || node->done || node->period < 0 || !node->fn || !strlen(node->name)) {
        DG_LOGW("[TASKER]", "tasker_enqueue fail, node is invalid");
        return TASK_PARA_ERR;
    }

    int ret = tasker_auto_init();
    if (ret != TASK_OK) return ret;

    ret = worker_sched_enqueue(node);
    if (ret != TASK_OK) return ret;

    node->fn = NULL;
    return TASK_OK;
}

void tasker_cancel_by_node(struct task_node* node) {
    if (!node) return;
    tasker_cancel_by_name(node->name);
}

void tasker_cancel_by_name(const char* name) {
    if (!name || !strlen(name)) return;

    pthread_mutex_lock(&(s_task_worker_ctx.s_sched_table->mtx));
    struct task_node* node = find_task_node_by_name(s_task_worker_ctx.s_sched_table->worker_queue, name);
    if (node) task_cancel(node);
    pthread_mutex_unlock(&(s_task_worker_ctx.s_sched_table->mtx));
}

int tasker_is_full(void) {
    /* 槽位指针由 sched 线程在自家锁内写:查询须持同一把锁,否则指针读竞态 */
    pthread_mutex_lock(&(s_task_worker_ctx.s_sched_table->mtx));
    int full = task_manager_is_full(s_task_worker_ctx.s_sched_table->worker_queue) ? 1 : 0;
    pthread_mutex_unlock(&(s_task_worker_ctx.s_sched_table->mtx));
    return full;
}

int tasker_is_empty(void) {
    pthread_mutex_lock(&(s_task_worker_ctx.s_sched_table->mtx));
    int empty = task_manager_is_empty(s_task_worker_ctx.s_sched_table->worker_queue) ? 1 : 0;
    pthread_mutex_unlock(&(s_task_worker_ctx.s_sched_table->mtx));
    return empty;
}

int tasker_task_init_li(struct task_node* out, int period, int run_cnt, 
                        const char* name, task_fn fn, void* ctx) {
    if (!out) return TASK_PARA_ERR;
    if (tasker_validate_params(period, fn, name) != TASK_OK) return TASK_PARA_ERR;
    if (tasker_auto_init() != TASK_OK) return TASK_INNER_ERR;

    task_node_init(out, LITTLE_TASK_DEFAULT_TIMEOUT, tasker_get_time_ms(), 
                   period, run_cnt, last, level_little, name, fn, ctx);
    return TASK_OK;
}

int tasker_task_init_mi(struct task_node* out, int period, int run_cnt, 
                        const char* name, task_fn fn, void* ctx) {
    if (!out) return TASK_PARA_ERR;
    if (tasker_validate_params(period, fn, name) != TASK_OK) return TASK_PARA_ERR;
    if (tasker_auto_init() != TASK_OK) return TASK_INNER_ERR;

    task_node_init(out, MIDDLE_TASK_DEFAULT_TIMEOUT, tasker_get_time_ms(), 
                   period, run_cnt, last, level_middle, name, fn, ctx);
    return TASK_OK;
}

int tasker_task_init_lo(struct task_node* out, int timeout, int period, 
                        int run_cnt, const char* name, task_fn fn, void* ctx) {
    if (!out) return TASK_PARA_ERR;
    if (tasker_validate_params(period, fn, name) != TASK_OK) return TASK_PARA_ERR;
    if (tasker_auto_init() != TASK_OK) return TASK_INNER_ERR;

    task_node_init(out, timeout, tasker_get_time_ms(), 
                   period, run_cnt, last, level_lots, name, fn, ctx);
    return TASK_OK;
}
