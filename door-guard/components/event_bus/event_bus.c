/*
 * event_bus.c — 事件总线核心实现
 *
 * 移植自模板 ovs/components/core/event_bus/event_bus.c(5.1 四修复版),
 * 保留其四个关键设计(注释即"为什么"):
 * 1. 锁外回调:dispatch 锁内快照匹配订阅者,锁外调用 handler——handler 内
 *    再订阅/退订/慢处理不会死锁或长期占锁;
 * 2. 事件内存池:定长块池 + 池满退化堆分配(头 reserved 作来源标志);
 * 3. 统计原子化:C11 atomic 计数;
 * 4. 事件名表供日志输出。
 * 与模板差异(见 README.md):mem_pool/logger 换为 dg_event_pool/dg_log,
 * 分发任务可停止 join(清理路径)。
 */
#include "event_bus.h"
#include "event_bus_port.h"
#include "dg_event_pool.h"
#include "dg_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "[EVENT_BUS]";

/** 上下文单例 */
static event_bus_context_t s_event_bus_ctx;

/** 订阅句柄池(静态分配,句柄轻量) */
static event_subscription_t s_subscription_pool[EVENT_BUS_MAX_SUBSCRIBERS];
static bool s_subscription_pool_used[EVENT_BUS_MAX_SUBSCRIBERS];

/** 事件内存池(init 时创建,常驻;deinit 销毁) */
static dg_event_pool_t *s_event_pool = NULL;

/** 池满退化堆分配计数(诊断) */
static atomic_uint s_heap_fallback_count;
/** 在途堆兜底块数(诊断:全部归还时应为 0) */
static atomic_uint s_heap_outstanding;

/** dispatch 快照上限:同事件并发匹配 handler 上限,超出仅计数告警 */
#define EVENT_SNAPSHOT_MAX  16

/* ---- 内部:事件对象创建/销毁 ---- */

event_bus_context_t *event_bus_get_context(void)
{
    return &s_event_bus_ctx;
}

event_t *event_bus_create_event(event_type_t type, const void *data, uint16_t data_len)
{
    if (data_len > 0 && data == NULL) {
        DG_LOGE(TAG, "create_event: data is NULL but data_len > 0");
        return NULL;
    }
    if (data_len > EVENT_BUS_MAX_EVENT_SIZE) {
        DG_LOGE(TAG, "create_event: data_len %u exceeds max %d",
                data_len, EVENT_BUS_MAX_EVENT_SIZE);
        return NULL;
    }

    size_t total_size = event_bus_calc_total_size(data_len);
    bool from_pool = false;
    event_t *event = NULL;

    if (s_event_pool) {
        event = dg_event_pool_alloc(s_event_pool);
        from_pool = (event != NULL);
    }
    if (event == NULL) {
        event = malloc(total_size);
        if (event == NULL) {
            DG_LOGE(TAG, "create_event: alloc failed, size=%zu", total_size);
            return NULL;
        }
        atomic_fetch_add(&s_heap_fallback_count, 1);
        atomic_fetch_add(&s_heap_outstanding, 1);
    }

    /* reserved 兼作分配来源标志:0=池,1=堆 */
    event->header.type = type;
    event->header.data_len = data_len;
    event->header.reserved = from_pool ? 0 : 1;
    event->header.timestamp = bus_now_ms();

    if (data != NULL && data_len > 0)
        memcpy(event->data, data, data_len);

    return event;
}

void event_bus_destroy_event(event_t *event)
{
    if (event == NULL)
        return;
    /* 带外归属判定:池归还时空闲链指针写进块首(覆盖事件头),块内标志
     * 不可靠,必须按地址范围判定归属(与模板同因) */
    if (s_event_pool && dg_event_pool_contains(s_event_pool, event)) {
        dg_event_pool_free(s_event_pool, event);
    } else {
        free(event);
        atomic_fetch_sub(&s_heap_outstanding, 1);
    }
}

/* ---- 分发:锁内快照、锁外回调(5.1 修复1) ---- */

void event_bus_dispatch_event(const event_t *event)
{
    if (event == NULL)
        return;

    event_bus_context_t *ctx = event_bus_get_context();
    struct {
        event_handler_fn_t handler;
        void *user_data;
    } snapshot[EVENT_SNAPSHOT_MAX];
    int handler_count = 0;
    int overflow = 0;

    if (bus_lock_take(ctx->lock, 100)) {
        for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
            subscriber_t *sub = &ctx->subscribers[i];
            if (sub->active && sub->event_type == event->header.type) {
                if (handler_count < EVENT_SNAPSHOT_MAX) {
                    snapshot[handler_count].handler = sub->handler;
                    snapshot[handler_count].user_data = sub->user_data;
                    handler_count++;
                } else {
                    overflow++;
                }
            }
        }
        bus_lock_give(ctx->lock);
    } else {
        /* 取锁失败宁可丢本条分发(发布方统计可见)也不带锁回调 */
        DG_LOGE(TAG, "dispatch_event: failed to take lock");
        return;
    }

    if (overflow > 0) {
        DG_LOGW(TAG, "dispatch_event: %d subscribers beyond snapshot cap %d (event 0x%08X)",
                overflow, EVENT_SNAPSHOT_MAX, event->header.type);
    }

    /* 锁外回调:再订阅/退订/发布均安全 */
    for (int i = 0; i < handler_count; i++) {
        int ret = snapshot[i].handler(event, snapshot[i].user_data);
        if (ret != 0) {
            atomic_fetch_add(&ctx->stats.handler_errors, 1);
            DG_LOGW(TAG, "dispatch_event: handler returned %d for event 0x%08X",
                    ret, event->header.type);
        }
    }

    atomic_fetch_add(&ctx->stats.events_processed, 1);
}

/* ---- 分发任务:停机位轮询使 deinit 可在 POLL_INTERVAL_MS 内 join ---- */

void event_bus_process_task(void *arg)
{
    (void)arg;
    event_bus_context_t *ctx = event_bus_get_context();
    queue_node_t node;

    while (!bus_task_should_stop()) {
        if (bus_queue_recv(ctx->queue, &node, EVENT_BUS_POLL_INTERVAL_MS)) {
            event_bus_dispatch_event(node.event);
            event_bus_destroy_event(node.event);
        }
    }
}

/* ---- 公共 API ---- */

event_bus_err_t event_bus_init(void)
{
    event_bus_context_t *ctx = event_bus_get_context();

    if (atomic_load(&ctx->initialized)) {
        DG_LOGW(TAG, "Event bus already initialized");
        return EVENT_BUS_ERR_ALREADY_INIT;
    }

    /* 池常驻:deinit 也保留,事件对象生命周期与总线一致 */
    if (s_event_pool == NULL) {
        s_event_pool = dg_event_pool_create(
            event_bus_calc_total_size(EVENT_BUS_MAX_EVENT_SIZE), EVENT_POOL_BLOCKS);
        if (s_event_pool == NULL) {
            DG_LOGE(TAG, "Failed to create event pool");
            return EVENT_BUS_ERR_NO_MEMORY;
        }
    }

    memset(ctx, 0, sizeof(event_bus_context_t));

    ctx->queue = bus_queue_create(EVENT_BUS_QUEUE_SIZE, (int)sizeof(queue_node_t));
    if (ctx->queue == NULL) {
        DG_LOGE(TAG, "Failed to create event queue");
        return EVENT_BUS_ERR_NO_MEMORY;
    }

    ctx->lock = bus_lock_create();
    if (ctx->lock == NULL) {
        DG_LOGE(TAG, "Failed to create lock");
        bus_queue_destroy(ctx->queue);
        ctx->queue = NULL;
        return EVENT_BUS_ERR_NO_MEMORY;
    }

    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        ctx->subscribers[i].active = false;
        ctx->subscribers[i].id = EVENT_BUS_INVALID_SUB_ID;
    }
    memset(s_subscription_pool_used, 0, sizeof(s_subscription_pool_used));

    ctx->next_subscriber_id = 1;    /* 0 保留为无效 ID */
    atomic_store(&s_heap_fallback_count, 0);
    atomic_store(&s_heap_outstanding, 0);
    atomic_store(&ctx->initialized, true);

    if (!bus_task_create("event_bus", EVENT_BUS_TASK_STACK_SIZE, 0,
                         event_bus_process_task, &ctx->task_handle)) {
        /* 失败回滚:任务起不来不能留下"半初始化"总线 */
        DG_LOGE(TAG, "Failed to create event bus task");
        bus_lock_destroy(ctx->lock);
        ctx->lock = NULL;
        bus_queue_destroy(ctx->queue);
        ctx->queue = NULL;
        atomic_store(&ctx->initialized, false);
        return EVENT_BUS_ERR_NO_MEMORY;
    }

    DG_LOGI(TAG, "Event bus initialized: queue=%d subscribers=%d pool=%dx%zuB",
            EVENT_BUS_QUEUE_SIZE, EVENT_BUS_MAX_SUBSCRIBERS,
            EVENT_POOL_BLOCKS, event_bus_calc_total_size(EVENT_BUS_MAX_EVENT_SIZE));
    return EVENT_BUS_OK;
}

event_bus_err_t event_bus_deinit(void)
{
    event_bus_context_t *ctx = event_bus_get_context();

    if (!atomic_load(&ctx->initialized))
        return EVENT_BUS_ERR_NOT_INIT;

    /* 先停任务再拆队列:任务还引用 queue/lock,顺序不能反 */
    if (ctx->task_handle != NULL) {
        bus_task_delete(ctx->task_handle);
        ctx->task_handle = NULL;
    }

    queue_node_t node;
    while (bus_queue_recv(ctx->queue, &node, 0))
        event_bus_destroy_event(node.event);

    bus_queue_destroy(ctx->queue);
    ctx->queue = NULL;
    bus_lock_destroy(ctx->lock);
    ctx->lock = NULL;

    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++)
        ctx->subscribers[i].active = false;
    ctx->subscriber_count = 0;
    atomic_store(&ctx->initialized, false);

    DG_LOGI(TAG, "Event bus deinitialized");
    return EVENT_BUS_OK;
}

event_bus_err_t event_bus_publish(event_type_t type, const void *data, size_t data_len)
{
    event_bus_context_t *ctx = event_bus_get_context();

    if (!atomic_load(&ctx->initialized)) {
        DG_LOGE(TAG, "publish: event bus not initialized");
        return EVENT_BUS_ERR_NOT_INIT;
    }
    if (data_len > EVENT_BUS_MAX_EVENT_SIZE) {
        DG_LOGE(TAG, "publish: data_len %zu exceeds max %d",
                data_len, EVENT_BUS_MAX_EVENT_SIZE);
        return EVENT_BUS_ERR_INVALID_PARAM;
    }
    if (data_len > 0 && data == NULL) {
        DG_LOGE(TAG, "publish: data is NULL but data_len > 0");
        return EVENT_BUS_ERR_INVALID_PARAM;
    }

    event_t *event = event_bus_create_event(type, data, (uint16_t)data_len);
    if (event == NULL) {
        DG_LOGE(TAG, "publish: failed to create event");
        return EVENT_BUS_ERR_NO_MEMORY;
    }

    queue_node_t node = {
        .event = event,
        .total_size = event_bus_calc_total_size((uint16_t)data_len),
    };

    if (!bus_queue_send(ctx->queue, &node)) {
        DG_LOGW(TAG, "publish: queue full, dropping event 0x%08X", type);
        event_bus_destroy_event(event);
        atomic_fetch_add(&ctx->stats.events_dropped, 1);
        return EVENT_BUS_ERR_QUEUE_FULL;
    }

    atomic_fetch_add(&ctx->stats.events_published, 1);
    return EVENT_BUS_OK;
}

static event_subscription_t *alloc_subscription(void)
{
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!s_subscription_pool_used[i]) {
            s_subscription_pool_used[i] = true;
            return &s_subscription_pool[i];
        }
    }
    return NULL;
}

static void free_subscription(event_subscription_t *sub)
{
    if (sub != NULL) {
        size_t index = sub - s_subscription_pool;
        if (index < EVENT_BUS_MAX_SUBSCRIBERS)
            s_subscription_pool_used[index] = false;
    }
}

event_subscription_t *event_bus_subscribe(event_type_t event_type,
                                          event_handler_fn_t handler,
                                          void *user_data)
{
    event_bus_context_t *ctx = event_bus_get_context();

    if (!atomic_load(&ctx->initialized)) {
        DG_LOGE(TAG, "subscribe: event bus not initialized");
        return NULL;
    }
    if (handler == NULL) {
        DG_LOGE(TAG, "subscribe: handler is NULL");
        return NULL;
    }

    if (!bus_lock_take(ctx->lock, 1000)) {
        DG_LOGE(TAG, "subscribe: failed to take lock");
        return NULL;
    }

    if (ctx->subscriber_count >= EVENT_BUS_MAX_SUBSCRIBERS) {
        DG_LOGE(TAG, "subscribe: max subscribers reached (%d)", EVENT_BUS_MAX_SUBSCRIBERS);
        bus_lock_give(ctx->lock);
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!ctx->subscribers[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        bus_lock_give(ctx->lock);
        return NULL;
    }

    event_subscription_t *sub = alloc_subscription();
    if (sub == NULL) {
        bus_lock_give(ctx->lock);
        return NULL;
    }

    subscriber_t *subscriber = &ctx->subscribers[slot];
    subscriber->event_type = event_type;
    subscriber->handler = handler;
    subscriber->user_data = user_data;
    subscriber->id = ctx->next_subscriber_id++;
    subscriber->active = true;

    sub->subscriber_id = subscriber->id;
    sub->event_type = event_type;
    ctx->subscriber_count++;

    bus_lock_give(ctx->lock);
    return sub;
}

event_bus_err_t event_bus_unsubscribe(event_subscription_t *subscription)
{
    event_bus_context_t *ctx = event_bus_get_context();

    if (!atomic_load(&ctx->initialized))
        return EVENT_BUS_ERR_NOT_INIT;
    if (subscription == NULL)
        return EVENT_BUS_ERR_INVALID_PARAM;

    if (!bus_lock_take(ctx->lock, 1000))
        return EVENT_BUS_ERR_TIMEOUT;

    bool found = false;
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        subscriber_t *sub = &ctx->subscribers[i];
        if (sub->active && sub->id == subscription->subscriber_id) {
            sub->active = false;
            sub->handler = NULL;
            sub->user_data = NULL;
            ctx->subscriber_count--;
            found = true;
            break;
        }
    }
    bus_lock_give(ctx->lock);

    free_subscription(subscription);

    if (!found) {
        DG_LOGW(TAG, "unsubscribe: subscriber id=%lu not found",
                (unsigned long)subscription->subscriber_id);
        return EVENT_BUS_ERR_NOT_FOUND;
    }
    return EVENT_BUS_OK;
}

event_bus_err_t event_bus_get_status(int *queue_size, int *subscriber_count)
{
    event_bus_context_t *ctx = event_bus_get_context();
    if (!atomic_load(&ctx->initialized))
        return EVENT_BUS_ERR_NOT_INIT;

    if (queue_size != NULL)
        *queue_size = bus_queue_count(ctx->queue);
    if (subscriber_count != NULL)
        *subscriber_count = ctx->subscriber_count;
    return EVENT_BUS_OK;
}

event_bus_err_t event_bus_get_stats(uint32_t *events_published, uint32_t *events_processed,
                                    uint32_t *events_dropped, uint32_t *handler_errors)
{
    event_bus_context_t *ctx = event_bus_get_context();
    if (!atomic_load(&ctx->initialized))
        return EVENT_BUS_ERR_NOT_INIT;

    if (events_published != NULL)
        *events_published = atomic_load(&ctx->stats.events_published);
    if (events_processed != NULL)
        *events_processed = atomic_load(&ctx->stats.events_processed);
    if (events_dropped != NULL)
        *events_dropped = atomic_load(&ctx->stats.events_dropped);
    if (handler_errors != NULL)
        *handler_errors = atomic_load(&ctx->stats.handler_errors);
    return EVENT_BUS_OK;
}

event_bus_err_t event_bus_reset_stats(void)
{
    event_bus_context_t *ctx = event_bus_get_context();
    if (!atomic_load(&ctx->initialized))
        return EVENT_BUS_ERR_NOT_INIT;

    atomic_store(&ctx->stats.events_published, 0);
    atomic_store(&ctx->stats.events_processed, 0);
    atomic_store(&ctx->stats.events_dropped, 0);
    atomic_store(&ctx->stats.handler_errors, 0);
    return EVENT_BUS_OK;
}

bool event_bus_is_initialized(void)
{
    return atomic_load(&event_bus_get_context()->initialized);
}

uint32_t event_bus_get_heap_fallback(void)
{
    return atomic_load(&s_heap_fallback_count);
}

uint32_t event_bus_get_heap_outstanding(void)
{
    return atomic_load(&s_heap_outstanding);
}

/* ---- 调试辅助 ---- */

static const struct {
    event_type_t type;
    const char *name;
} s_event_type_names[] = {
    { EVENT_SYSTEM_STARTUP, "SYSTEM_STARTUP" },
    { EVENT_SYSTEM_SHUTDOWN, "SYSTEM_SHUTDOWN" },
    { EVENT_SYSTEM_ERROR, "SYSTEM_ERROR" },
    { EVENT_UI_PAGE_CHANGE, "UI_PAGE_CHANGE" },
    { EVENT_UI_REFRESH_REQUEST, "UI_REFRESH_REQUEST" },
    { EVENT_TEST_PUBLISH, "TEST_PUBLISH" },
    { EVENT_TEST_NO_DATA, "TEST_NO_DATA" },
    { EVENT_TEST_REENTRANT, "TEST_REENTRANT" },
    { EVENT_TEST_POOL, "TEST_POOL" },
    { EVENT_TYPE_MAX, NULL },
};

const char *event_bus_get_type_name(event_type_t type)
{
    for (int i = 0; s_event_type_names[i].name != NULL; i++) {
        if (s_event_type_names[i].type == type)
            return s_event_type_names[i].name;
    }
    return "UNKNOWN";
}

const char *event_bus_get_err_name(event_bus_err_t err)
{
    switch (err) {
    case EVENT_BUS_OK:                  return "OK";
    case EVENT_BUS_ERR_INVALID_PARAM:   return "INVALID_PARAM";
    case EVENT_BUS_ERR_NOT_INIT:        return "NOT_INIT";
    case EVENT_BUS_ERR_ALREADY_INIT:    return "ALREADY_INIT";
    case EVENT_BUS_ERR_QUEUE_FULL:      return "QUEUE_FULL";
    case EVENT_BUS_ERR_NO_MEMORY:       return "NO_MEMORY";
    case EVENT_BUS_ERR_NOT_FOUND:       return "NOT_FOUND";
    case EVENT_BUS_ERR_MAX_SUBSCRIBERS: return "MAX_SUBSCRIBERS";
    case EVENT_BUS_ERR_TIMEOUT:         return "TIMEOUT";
    case EVENT_BUS_ERR_INTERNAL:        return "INTERNAL";
    default:                            return "UNKNOWN";
    }
}

void event_bus_print_status(void)
{
    event_bus_context_t *ctx = event_bus_get_context();

    printf("=== Event Bus Status ===\n");
    printf("Initialized: %s\n", ctx->initialized ? "Yes" : "No");
    if (atomic_load(&ctx->initialized)) {
        int queue_size = 0, subscriber_count = 0;
        event_bus_get_status(&queue_size, &subscriber_count);
        printf("Queue: %d / %d\n", queue_size, EVENT_BUS_QUEUE_SIZE);
        printf("Subscribers: %d / %d\n", subscriber_count, EVENT_BUS_MAX_SUBSCRIBERS);
        printf("Stats: published=%u processed=%u dropped=%u handler_errors=%u\n",
               atomic_load(&ctx->stats.events_published),
               atomic_load(&ctx->stats.events_processed),
               atomic_load(&ctx->stats.events_dropped),
               atomic_load(&ctx->stats.handler_errors));
    }
    printf("========================\n");
}
