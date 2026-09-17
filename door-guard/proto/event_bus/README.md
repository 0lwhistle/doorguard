# event_bus — 事件总线(UI ↔ 服务解耦骨干)

## 出处

ESP32 模板 `ovs` 的 `components/core/event_bus/`(event_bus.c / event_bus_types.h /
event_bus_internal.h / event_bus_port.*,2026-09-09 的 REFACTORING_PLAN 5.1 四修复版)。
模板路径:`/home/olwhistle/dockerNow/esp32/programs/ovs`。

## port 改动点(pthread/Linux)

| 项 | 模板 | 本移植 | 原因 |
|---|---|---|---|
| 平台分支 | ESP(FreeRTOS)+ PC 双分支 | 仅保留 pthread 分支 | door-guard 只跑 Linux |
| 内存池 | 依赖模板 mem_pool 模块 | `dg_event_pool.{h,c}` 自实现(同语义:定长块 + 空闲链 + 池满退化堆) | 只此一处用池,不值得引入通用池 |
| 日志 | 模板 logger.h | `proto/dg_log.h` | door-guard 统一日志尚未建,先承接 |
| 锁获取 | PC 分支 trylock+nanosleep 轮询 | `pthread_mutex_timedlock` | 语义相同,不烧 CPU |
| 任务停止 | PC 分支任务永不退出,deinit 后句柄泄漏 | stop 标志 + join,deinit 在 ≤10ms 内回收线程 | door-guard 纪律:中途退出有清理路径 |
| 事件域 | WiFi/LoRa/Audio 等模板业务事件 | 按门禁模块重组 SYSTEM/UI/TEST 域;业务事件(EV_*)在 Phase 2 proto/events.h 定义 | 域不同 |
| 栈大小宏 | 按字节常量(FreeRTOS 语义) | `EVENT_BUS_TASK_STACK_SIZE` 提高到 64KB 仅作 attr 提示 | pthread 栈按需补页 |

核心机制**未动**(API 逐个一致):锁外回调、事件池 + 堆兜底(reserved 作来源标志)、
原子统计、队列满丢弃计数、订阅句柄池。

## 使用示例

```c
#include "event_bus.h"

/* 1. 订阅:handler 在分发线程执行,必须短小;耗时操作转投自身线程 */
static int on_auth_result(const event_t *e, void *ud) {
    const auth_evt_t *d = (const auth_evt_t *)e->data;
    /* ... 刷新 UI ... */
    return 0;                                   /* 非 0 计入 handler_errors */
}
event_bus_init();
event_subscription_t *sub = event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);

/* 2. 发布:数据被拷贝,返回后调用方可立即复用/释放 */
auth_evt_t evt = { .result = 0 };
EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &evt);        /* 类型安全,自动 sizeof */
EVENT_BUS_PUBLISH_EMPTY(EVENT_SYSTEM_STARTUP);  /* 无载荷 */

/* 3. 退订/清理 */
event_bus_unsubscribe(sub);
event_bus_deinit();
```

完整可运行示例:`demo_event_bus.c`(ctest 里作为冒烟用例跑通)。

## 注意事项

- 队列深 32、发布非阻塞:发布方拿到 `EVENT_BUS_ERR_QUEUE_FULL` 要么接受丢弃
  (低价值事件)要么短暂重试(高价值事件,见 tests/test_event_bus_stress.c)。
- `EVENT_BUS_MAX_EVENT_SIZE` 256B:更大的数据(如特征向量)走 storage,事件里传句柄/ID。
- handler 里不要等锁做重活:分发线程被卡住会级联阻塞所有事件。
