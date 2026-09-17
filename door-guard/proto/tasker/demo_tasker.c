/*
 * demo_tasker.c — tasker 使用示例(可执行,ctest 冒烟跑通)
 *
 * 演示 door-guard 服务层的标准短任务用法:周期任务(心跳式)+ 一次性
 * 延迟任务 + 按名取消。
 */
#include "tasker.h"

#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_int s_ticks = 0;

/* 周期任务:模拟门磁轮询(≤500ms 短任务约定) */
static enum task_t tick(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_ticks, 1);
    return TASK_OK;
}

static atomic_int s_one_shot_ran = 0;

static enum task_t one_shot(void *ctx)
{
    (void)ctx;
    atomic_store(&s_one_shot_ran, 1);
    return TASK_OK;
}

int main(void)
{
    if (tasker_init() != TASK_OK) {
        fprintf(stderr, "demo: tasker_init 失败\n");
        return 1;
    }

    struct task_node periodic, delayed;
    if (tasker_task_init_li(&periodic, 20, 10, "demo_tick", tick, NULL) != TASK_OK ||
        tasker_enqueue(&periodic) != TASK_OK) {
        fprintf(stderr, "demo: 周期任务入队失败\n");
        return 1;
    }
    /* 一次性:period=0 + run_cnt=1,下一调度周期即执行(演示"延迟动作") */
    if (tasker_task_init_li(&delayed, 0, 1, "demo_once", one_shot, NULL) != TASK_OK ||
        tasker_enqueue(&delayed) != TASK_OK) {
        fprintf(stderr, "demo: 一次性任务入队失败\n");
        return 1;
    }

    /* 10 次 × 20ms = 200ms,给 2s 余量 */
    for (int i = 0; i < 2000 && atomic_load(&s_ticks) < 10; i++)
        usleep(1000);
    for (int i = 0; i < 1000 && !atomic_load(&s_one_shot_ran); i++)
        usleep(1000);

    tasker_cancel_by_name("demo_tick");

    if (atomic_load(&s_ticks) != 10 || !atomic_load(&s_one_shot_ran)) {
        fprintf(stderr, "demo: ticks=%d one_shot=%d\n",
                atomic_load(&s_ticks), atomic_load(&s_one_shot_ran));
        return 1;
    }
    printf("demo_tasker: OK (ticks=%d, one_shot ran)\n", atomic_load(&s_ticks));
    return 0;
}
