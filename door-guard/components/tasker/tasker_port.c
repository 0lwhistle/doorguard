/*
 * tasker_port.c — tasker 平台移植实现(Linux/pthread)
 *
 * 移植自模板 ovs/components/core/tasker/tasker_port.c PC 分支。
 * 模板的超时定时器在 Linux 移植里恒为 no-op(只做"升级 level"标记、
 * 不能中断任务函数),PC/板上都不依赖——整段已删(2026-10-25 B6-5);
 * door-guard 业务超时(弹窗 5s 等)由状态机 timer_seq 机制实现。
 */
#include "tasker_port.h"

#include <time.h>

uint64_t tasker_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
