/*
 * demo_event_bus.c — event_bus 使用示例(可执行,ctest 冒烟跑通)
 *
 * 演示 door-guard 模块间通信的标准姿势:
 *   服务层(AccessService)发布认证结果 → UI 层订阅刷新弹窗,互不直接调用。
 */
#include "event_bus.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* 载荷结构:真实业务中定义在 proto/events.h(Phase 2);demo 就地示意 */
typedef struct {
    char user_id[32];
    int result;                 /* 0 通过 1 拒绝 */
} demo_auth_evt_t;

static int on_auth_result(const event_t *event, void *user_data)
{
    /* handler 在分发线程执行,主线程轮询同一变量:必须原子(否则 tsan 报竞争) */
    atomic_int *received = user_data;
    const demo_auth_evt_t *d = (const demo_auth_evt_t *)event->data;
    printf("demo: 收到认证结果 user=%s result=%d ts=%u\n",
           d->user_id, d->result, event->header.timestamp);
    atomic_fetch_add(received, 1);
    return 0;
}

int main(void)
{
    if (event_bus_init() != EVENT_BUS_OK) {
        fprintf(stderr, "demo: event_bus_init 失败\n");
        return 1;
    }

    atomic_int received = 0;
    event_subscription_t *sub =
        event_bus_subscribe(EVENT_TEST_PUBLISH, on_auth_result, &received);
    if (sub == NULL) {
        fprintf(stderr, "demo: 订阅失败\n");
        return 1;
    }

    demo_auth_evt_t evt;
    strncpy(evt.user_id, "10001", sizeof(evt.user_id) - 1);
    evt.result = 0;
    if (EVENT_BUS_PUBLISH(EVENT_TEST_PUBLISH, &evt) != EVENT_BUS_OK) {
        fprintf(stderr, "demo: 发布失败\n");
        return 1;
    }

    /* 分发是异步的:轮询等待(真实代码由业务驱动,不这样等) */
    for (int i = 0; i < 2000 && atomic_load(&received) == 0; i++) {
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    event_bus_unsubscribe(sub);
    event_bus_deinit();

    if (atomic_load(&received) != 1) {
        fprintf(stderr, "demo: 未收到事件(received=%d)\n", atomic_load(&received));
        return 1;
    }
    printf("demo: OK\n");
    return 0;
}
