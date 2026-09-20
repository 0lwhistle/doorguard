/*
 * tasker.h — 短任务调度器公共 API
 *
 * 移植自 ESP32 模板 ovs/components/api/tasker_api/tasker.h(实现位于本目录,
 * task_manager/task_worker/tasker_port;出处与改动点见本目录 README.md)。
 *
 * 使用约定(模板 REFACTORING_PLAN 5.2,违反将导致调度失真):
 * 1. 长任务(>1s)禁止进 tasker,须自建 pthread( audio/web/lvgl 均如此);
 *    tasker 仅收 ≤500ms 短周期任务(心跳/超时/轮询);
 * 2. priority 维度(first/middle/last)已废弃,仅 level
 *    (little/middle/lots 时间成本分级)有效;
 * 3. 超时仅能升级任务 level,不能中断任务函数本身。
 */
#ifndef TASKER_H
#define TASKER_H

#include "task_manager.h"

#define TASK_CNT_INF -1

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化调度系统(1 个调度线程 + 1 个分发线程 + 3 个分级执行线程)。
 *  首次调用其他 API 时也会自动初始化。 */
int tasker_init(void);

/** 入队任务节点(移动语义):成功后源节点被作废(fn 置 NULL);失败源节点
 *  保持原样可重试。返回 TASK_OK / TASK_PARA_ERR / TASK_QUEUE_FULL / ... */
int tasker_enqueue(struct task_node *node);

/** 按节点指针取消(只取消调度表中已入队的副本,不影响源节点) */
void tasker_cancel_by_node(struct task_node *node);

/** 按名字取消(名字须唯一) */
void tasker_cancel_by_name(const char *name);

/** 调度表状态查询:1 真 / 0 假 */
int tasker_is_full(void);
int tasker_is_empty(void);

/* ---- 调用方栈上初始化任务节点(无堆分配),按时间成本分级 ---- */

/** 高频短任务(默认超时 50ms) */
int tasker_task_init_li(struct task_node *out, const int period, const int run_cnt,
                        const char *name, task_fn fn, void *ctx);
/** 常规任务(默认超时 1000ms) */
int tasker_task_init_mi(struct task_node *out, const int period, const int run_cnt,
                        const char *name, task_fn fn, void *ctx);
/** 重任务(显式超时) */
int tasker_task_init_lo(struct task_node *out, const int timeout, const int period,
                        const int run_cnt, const char *name, task_fn fn, void *ctx);

/*
 * 使用示例:
 * @code
 * static enum task_t poll_door_sensor(void *ctx) {  // 周期 200ms,无限次
 *     // ... 读 GPIO,变化则 EVENT_BUS_PUBLISH ...
 *     return TASK_OK;    // 返回非 OK 时调度器会重试本周期
 * }
 * struct task_node node;
 * tasker_task_init_li(&node, 200, TASK_CNT_INF, "door_poll", poll_door_sensor, NULL);
 * tasker_enqueue(&node);
 * tasker_cancel_by_name("door_poll");   // 停止(名字须与入队时一致)
 * @endcode
 */

#ifdef __cplusplus
}
#endif

#endif /* TASKER_H */
