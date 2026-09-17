/*
 * event_bus.h — 事件总线公共 API
 *
 * 移植自 ESP32 模板 ovs/components/core/event_bus/event_bus.h(出处与
 * port 改动点见本目录 README.md);使用示例见文件尾与本目录 demo_event_bus.c。
 *
 * 机制:
 * - 发布-订阅,按事件类型分发;订阅/发布线程安全
 * - 独立分发线程异步调用 handler(锁外回调,handler 内再订阅/发布安全)
 * - 事件数据拷贝进柔性数组,发布后调用方可立即释放
 * - 队列满时发布失败返回 QUEUE_FULL 并计数,不阻塞发布方
 */
#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include "event_bus_types.h"
#include "event_bus_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */

/** 初始化:建队列/池/分发线程。重复调用返回 ALREADY_INIT。
 *  必须先于一切 publish/subscribe,失败时各资源已回滚(无半初始化)。 */
event_bus_err_t event_bus_init(void);

/** 反初始化:停止分发线程(最长 POLL_INTERVAL_MS 内退出)、清队列、释放资源 */
event_bus_err_t event_bus_deinit(void);

/* ---- 发布 ---- */

/**
 * 发布事件(线程安全,非阻塞)。
 * @param type      事件类型
 * @param data      载荷指针,可为 NULL(data_len 须为 0)
 * @param data_len  载荷字节数,≤ EVENT_BUS_MAX_EVENT_SIZE
 * @return OK / NOT_INIT / INVALID_PARAM / QUEUE_FULL / NO_MEMORY
 */
event_bus_err_t event_bus_publish(event_type_t type, const void *data, size_t data_len);

/* ---- 订阅 ---- */

/**
 * 订阅事件(线程安全)。同一类型可多订阅者,同一函数可订阅多类型。
 * @return 订阅句柄(供 unsubscribe);NULL 失败(参数非法/未初始化/超上限)
 * @note handler 在分发线程执行,应短小;阻塞耗时操作请转投自身线程
 */
event_subscription_t *event_bus_subscribe(event_type_t event_type,
                                          event_handler_fn_t handler,
                                          void *user_data);

/** 取消订阅;句柄随即失效。重复取消同一句柄返回 NOT_FOUND */
event_bus_err_t event_bus_unsubscribe(event_subscription_t *subscription);

/* ---- 状态与统计(诊断) ---- */

event_bus_err_t event_bus_get_status(int *queue_size, int *subscriber_count);
event_bus_err_t event_bus_get_stats(uint32_t *events_published, uint32_t *events_processed,
                                    uint32_t *events_dropped, uint32_t *handler_errors);
event_bus_err_t event_bus_reset_stats(void);
bool event_bus_is_initialized(void);

/** 池满退化堆分配的累计次数(诊断) */
uint32_t event_bus_get_heap_fallback(void);

/** 当前在途的堆兜底块数:压测后应为 0(全部归还) */
uint32_t event_bus_get_heap_outstanding(void);

/* ---- 调试辅助 ---- */

const char *event_bus_get_type_name(event_type_t type);
const char *event_bus_get_err_name(event_bus_err_t err);
void event_bus_print_status(void);

/*
 * 使用示例:
 * @code
 * static int on_auth(const event_t *e, void *ud) {
 *     const auth_evt_t *d = (const auth_evt_t *)e->data;   // 模板惯例:载荷直接取 data
 *     printf("user=%s result=%d\n", d->user_id, d->result);
 *     return 0;
 * }
 *
 * event_bus_init();
 * event_subscription_t *sub = event_bus_subscribe(EV_AUTH_RESULT, on_auth, NULL);
 * auth_evt_t payload = { .result = 0 };
 * EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &payload);
 * ...
 * event_bus_unsubscribe(sub);
 * event_bus_deinit();
 * @endcode
 */

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
