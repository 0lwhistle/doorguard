/*
 * event_bus_internal.h — 事件总线内部结构(仅 event_bus.c 使用)
 *
 * 移植自模板 ovs/components/core/event_bus/event_bus_internal.h;
 * 与模板差异:去掉 esp_task_wdt 相关注释,其余结构保持一致。
 */
#ifndef EVENT_BUS_INTERNAL_H
#define EVENT_BUS_INTERNAL_H

#include "event_bus_types.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 处理函数:返回 0 成功,非 0 计入 handler_errors */
typedef int (*event_handler_fn_t)(const event_t *event, void *user_data);

typedef struct {
    event_type_t event_type;
    event_handler_fn_t handler;
    void *user_data;
    uint32_t id;
    bool active;
} subscriber_t;

/** 取消订阅用的句柄(订阅返回值) */
typedef struct {
    uint32_t subscriber_id;
    event_type_t event_type;
} event_subscription_t;

/** 队列节点:事件指针 + 总大小(大小预留给出错诊断) */
typedef struct {
    event_t *event;
    size_t total_size;
} queue_node_t;

typedef struct {
    void *queue;                /**< port 层队列句柄 */
    void *task_handle;          /**< port 层任务句柄 */

    subscriber_t subscribers[EVENT_BUS_MAX_SUBSCRIBERS];
    uint32_t next_subscriber_id;
    int subscriber_count;

    void *lock;                 /**< port 层互斥锁句柄 */
    volatile bool stop;         /**< deinit 置位,分发任务据此退出(port 改动点) */
    bool initialized;

    struct {
        atomic_uint events_published;
        atomic_uint events_processed;
        atomic_uint events_dropped;
        atomic_uint handler_errors;
    } stats;                    /**< C11 原子计数:多线程无锁(模板 5.1 修复项3) */
} event_bus_context_t;

event_bus_context_t *event_bus_get_context(void);
void event_bus_process_task(void *arg);
void event_bus_dispatch_event(const event_t *event);

static inline size_t event_bus_calc_total_size(uint16_t data_len)
{
    return sizeof(event_t) + data_len;
}

event_t *event_bus_create_event(event_type_t type, const void *data, uint16_t data_len);
void event_bus_destroy_event(event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_INTERNAL_H */
