/*
 * test_event_bus.c — event_bus 移植测试
 *
 * 移植自模板 tests/test_event_bus.c(EB1/EB2/EB4/EB5 断言逐条保留,未弱化;
 * EB3 池耗尽用例中模板 mem_stat_get 断言换为本移植 dg_event_pool 的
 * "在途堆块数归零"断言,强度等价)。模板 WiFi/Sensor 事件名按 door-guard
 * 事件域映射:TEST_PUBLISH / TEST_NO_DATA / TEST_REENTRANT / TEST_POOL。
 */
#include "event_bus.h"
#include "dg_test.h"

#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

static void wait_until(atomic_int *flag, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i += 2) {
        if (atomic_load(flag))
            return;
        usleep(2000);
    }
}

/* ---------- EB1 生命周期 / 发布 / 载荷逐字段完整性 ---------- */
static atomic_int s_got = 0;
static event_type_t s_got_type = 0;
static uint8_t s_got_data[EVENT_BUS_MAX_EVENT_SIZE];
static uint16_t s_got_len = 0;

static int on_basic(const event_t *e, void *ud)
{
    (void)ud;
    s_got_type = e->header.type;
    s_got_len = e->header.data_len;
    if (e->header.data_len > 0)
        memcpy(s_got_data, e->data, e->header.data_len);
    atomic_store(&s_got, 1);
    return 0;
}

static void test_basic(void)
{
    printf("[EB1] lifecycle / publish / payload integrity\n");
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(event_bus_is_initialized());
    DG_CHECK(event_bus_init() == EVENT_BUS_ERR_ALREADY_INIT);

    atomic_store(&s_got, 0);
    DG_CHECK(event_bus_subscribe(EVENT_TEST_PUBLISH, on_basic, NULL) != NULL);

    event_test_payload_t data = { 0 };
    data.rssi = -55;
    data.channel = 6;
    DG_CHECK(event_bus_publish(EVENT_TEST_PUBLISH, &data, sizeof(data)) == EVENT_BUS_OK);
    wait_until(&s_got, 2000);
    DG_CHECK(atomic_load(&s_got) == 1);
    DG_CHECK(s_got_type == EVENT_TEST_PUBLISH);
    /* 载荷逐字段相等(模板此处只看类型;门禁契约要求逐字段,加严不弱化) */
    DG_CHECK(s_got_len == sizeof(data));
    event_test_payload_t got = { 0 };
    memcpy(&got, s_got_data, sizeof(got));
    DG_CHECK(got.rssi == -55 && got.channel == 6);

    /* 无数据事件:订阅按类型过滤 */
    DG_CHECK(event_bus_subscribe(EVENT_TEST_NO_DATA, on_basic, NULL) != NULL);
    atomic_store(&s_got, 0);
    DG_CHECK(event_bus_publish(EVENT_TEST_NO_DATA, NULL, 0) == EVENT_BUS_OK);
    wait_until(&s_got, 2000);
    DG_CHECK(atomic_load(&s_got) == 1 && s_got_type == EVENT_TEST_NO_DATA);
    DG_CHECK(s_got_len == 0);

    /* 参数校验:data 为 NULL 但长度非 0 / 超最大尺寸 */
    DG_CHECK(event_bus_publish(EVENT_TEST_PUBLISH, NULL, 8) == EVENT_BUS_ERR_INVALID_PARAM);
    DG_CHECK(event_bus_publish(EVENT_TEST_PUBLISH, &data, EVENT_BUS_MAX_EVENT_SIZE + 1)
             == EVENT_BUS_ERR_INVALID_PARAM);

    uint32_t pub = 0, pro = 0, drop = 0, err = 0;
    DG_CHECK(event_bus_get_stats(&pub, &pro, &drop, &err) == EVENT_BUS_OK);
    DG_CHECK(pub >= 2 && pro >= 2);
}

/* ---------- EB2 锁外回调:handler 内再订阅/发布/退订不死锁(5.1 修复1) ---------- */
static atomic_int s_nested_done = 0;

static int on_nested(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    atomic_store(&s_nested_done, 1);
    return 0;
}

static event_subscription_t *s_self_sub = NULL;

static int on_reentrant(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    /* 危险动作三连:分发过程中再订阅、发布、退订自己 */
    event_bus_subscribe(EVENT_TEST_NO_DATA, on_nested, NULL);
    event_bus_publish(EVENT_UI_PAGE_CHANGE, NULL, 0);
    event_bus_unsubscribe(s_self_sub);
    atomic_store(&s_nested_done, 1);
    return 0;
}

static void test_reentrant_dispatch(void)
{
    printf("[EB2] reentrant dispatch (no deadlock)\n");
    atomic_store(&s_nested_done, 0);
    s_self_sub = event_bus_subscribe(EVENT_TEST_REENTRANT, on_reentrant, NULL);
    DG_CHECK(s_self_sub != NULL);

    DG_CHECK(event_bus_publish(EVENT_TEST_REENTRANT, NULL, 0) == EVENT_BUS_OK);
    wait_until(&s_nested_done, 3000);
    DG_CHECK(atomic_load(&s_nested_done) == 1);    /* 锁内回调会死锁挂起 */
    usleep(100000);                                /* 给嵌套订阅的事件处理留时间 */
}

/* ---------- EB3 池耗尽 → 堆兜底(5.1 修复2) ---------- */
static void test_pool_fallback(void)
{
    printf("[EB3] pool fallback (%d blocks, heap overflow counted)\n", EVENT_POOL_BLOCKS);
    uint32_t fallback_before = event_bus_get_heap_fallback();
    uint32_t outstanding_before = event_bus_get_heap_outstanding();

    /* 绕过 32 深队列直接创建 60 个事件:池 48 + 堆兜底 12 */
    event_t *evts[60];
    int made = 0;
    for (int i = 0; i < 60; i++) {
        evts[i] = event_bus_create_event(EVENT_TEST_POOL, NULL, 0);
        if (evts[i])
            made++;
    }
    DG_CHECK(made == 60);
    DG_CHECK(event_bus_get_heap_fallback() - fallback_before >= 12);

    for (int i = 0; i < made; i++)
        event_bus_destroy_event(evts[i]);
    /* 模板断言 mem_stat 归零的等价:堆兜底块全部归还,在途数回落原值 */
    DG_CHECK(event_bus_get_heap_outstanding() == outstanding_before);
}

/* ---------- EB4 事件名表覆盖(5.1 修复4) ---------- */
static void test_name_table(void)
{
    printf("[EB4] name table covers event types (spot check)\n");
    const event_type_t samples[] = {
        EVENT_SYSTEM_STARTUP, EVENT_SYSTEM_SHUTDOWN, EVENT_SYSTEM_ERROR,
        EVENT_UI_PAGE_CHANGE, EVENT_UI_REFRESH_REQUEST,
        EVENT_TEST_PUBLISH, EVENT_TEST_NO_DATA, EVENT_TEST_REENTRANT, EVENT_TEST_POOL,
    };
    int unknown = 0;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        if (strcmp(event_bus_get_type_name(samples[i]), "UNKNOWN") == 0) {
            unknown++;
            printf("  UNKNOWN for type 0x%08X\n", samples[i]);
        }
    }
    DG_CHECK(unknown == 0);
}

/* ---------- EB5 统计与错误计数 / 退订语义 ---------- */
static atomic_int s_err_got = 0;

static int on_err(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    atomic_store(&s_err_got, 1);
    return -1;                                  /* 模拟 handler 失败 */
}

static void test_stats(void)
{
    printf("[EB5] stats / handler error counting / unsubscribe\n");
    DG_CHECK(event_bus_reset_stats() == EVENT_BUS_OK);

    event_subscription_t *sub = event_bus_subscribe(EVENT_SYSTEM_ERROR, on_err, NULL);
    DG_CHECK(sub != NULL);

    atomic_store(&s_err_got, 0);
    DG_CHECK(event_bus_publish(EVENT_SYSTEM_ERROR, NULL, 0) == EVENT_BUS_OK);
    wait_until(&s_err_got, 2000);

    uint32_t pub = 0, pro = 0, drop = 0, herr = 0;
    event_bus_get_stats(&pub, &pro, &drop, &herr);
    DG_CHECK(pub == 1 && pro == 1);
    DG_CHECK(herr == 1);                        /* 非 0 返回被计数 */

    DG_CHECK(event_bus_unsubscribe(sub) == EVENT_BUS_OK);
    DG_CHECK(event_bus_unsubscribe(sub) == EVENT_BUS_ERR_NOT_FOUND);
    DG_CHECK(event_bus_subscribe(EVENT_SYSTEM_ERROR, NULL, NULL) == NULL);

    /* 退订后不再投递 */
    atomic_store(&s_err_got, 0);
    event_bus_publish(EVENT_SYSTEM_ERROR, NULL, 0);
    usleep(100000);
    DG_CHECK(atomic_load(&s_err_got) == 0);
}

int main(void)
{
    test_basic();
    test_reentrant_dispatch();
    test_pool_fallback();
    test_name_table();
    test_stats();

    /* 清理路径:deinit 后 publish/subscribe 显式失败 */
    DG_CHECK(event_bus_deinit() == EVENT_BUS_OK);
    DG_CHECK(event_bus_deinit() == EVENT_BUS_ERR_NOT_INIT);
    DG_CHECK(event_bus_publish(EVENT_SYSTEM_ERROR, NULL, 0) == EVENT_BUS_ERR_NOT_INIT);
    DG_CHECK(event_bus_subscribe(EVENT_SYSTEM_ERROR, on_err, NULL) == NULL);

    DG_TEST_EXIT();
}
