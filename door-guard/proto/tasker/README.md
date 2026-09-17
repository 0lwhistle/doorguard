# tasker — 短任务调度器

## 出处

ESP32 模板 `ovs` 的 `components/api/tasker_api/tasker.h` + `components/core/tasker/`
(tasker.c / task_manager.* / task_worker.* / tasker_port.*,2026-09-09 的
REFACTORING_PLAN 5.2 修复版)。模板路径:`/home/olwhistle/dockerNow/esp32/programs/ovs`。

## 结构与线程模型

- **sched 线程**:按最近截止时间 cond_timedwait 休眠(空闲零 CPU),到期任务
  指针移交 dispatcher;一次性(period=0)立即派发,周期任务按 period 轮转
- **dispatcher 线程**:按 level 把节点分发给执行线程;队列满则优先级提升 + 重排
- **little/middle/lots 三个执行线程**:按时间成本分级执行任务函数
- 节点从静态节点池分配(64 个),`tasker_enqueue` 为移动语义(入队后源节点作废)

## port 改动点

| 项 | 模板 | 本移植 | 原因 |
|---|---|---|---|
| 平台分支 | ESP + PC 双实现 | 仅 PC(pthread)分支 | door-guard 只跑 Linux |
| logger.h | 模板私有日志 | `proto/dg_log.h` | 统一日志承接 |
| mem.h | 模板私有内存管理 | 标准 malloc/free | 门禁内存充裕,无池化必要 |
| 线程栈宏 | 3~8KB(FreeRTOS 静态栈) | 64~128KB(attr 提示) | pthread 栈按需补页 |
| 超时定时器 | PC 分支 no-op | 保持 no-op(同模板) | 超时机制只升级 level 不中断任务;业务超时走状态机 timer_seq,不依赖此处 |

调度核心逻辑(5.2 三修复:cond 超时等待替代忙轮询、节点原子化、失败路径
资源释放;自旋熔断;节点池;计数排序)**逐行保留**。

## 使用约定(违反会调度失真,详见 tasker.h 头注)

1. 只收 **≤500ms** 短任务:心跳/超时检查/轻量轮询;长任务(音频/web/LVGL/
   摄像头)自建 pthread
2. 超时不能中断任务函数本身,只会升级任务 level
3. 任务名唯一(按名取消依赖它)

## 使用示例

```c
#include "tasker.h"

static enum task_t poll_door_sensor(void *ctx) {   /* 周期 200ms,无限次 */
    /* 读 GPIO;变化则发 EVENT_BUS_PUBLISH(与 event_bus 解耦协作) */
    return TASK_OK;
}

struct task_node node;
tasker_task_init_li(&node, 200, TASK_CNT_INF, "door_poll", poll_door_sensor, NULL);
tasker_enqueue(&node);                 /* 移动语义:node 被调度表接管 */

tasker_cancel_by_name("door_poll");    /* 一次性延迟任务:period=0, run_cnt=1 */
```

完整可运行示例:`demo_tasker.c`(ctest 冒烟)。

## 注意事项

- 任务函数返回非 TASK_OK 时调度器按"本周期失败"处理并重试;致命错误请在
  函数内自行上报(发事件)后返回 TASK_OK,避免空转重试
- 调度表容量:sched 64 / dispatcher 64 / 分级队列各 8,满时返回
  TASK_QUEUE_FULL 或优先级提升等待,不丢弃任务
