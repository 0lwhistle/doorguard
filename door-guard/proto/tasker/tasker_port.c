/*
 * tasker_port.c — tasker 平台移植实现(Linux/pthread)
 *
 * 移植自模板 ovs/components/core/tasker/tasker_port.c PC 分支。
 * Linux 上超时定时器为 no-op 的原因:模板超时机制只做"升级 level"标记、
 * 不能中断任务函数(见 tasker.h 约定3),PC/板上都不依赖真实定时器;
 * door-guard 业务超时(弹窗 5s 等)由状态机 timer_seq 机制实现,不走这里。
 */
#include "tasker_port.h"

#include <stdlib.h>
#include <time.h>

uint64_t tasker_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int tasker_timer_create(tasker_timer_t *timer, void (*cb)(void *), void *arg)
{
    (void)cb; (void)arg;
    *timer = NULL;
    return 0;
}

int tasker_timer_start_once(tasker_timer_t timer, uint64_t us)
{
    (void)timer; (void)us;
    return 0;
}

void tasker_timer_stop(tasker_timer_t timer)
{
    (void)timer;
}

void tasker_timer_delete(tasker_timer_t timer)
{
    (void)timer;
}
