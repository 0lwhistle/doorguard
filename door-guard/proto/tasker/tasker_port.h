/*
 * tasker_port.h — tasker 平台移植层(内部头)
 *
 * 移植自模板 ovs/components/core/tasker/tasker_port.h;door-guard 只跑
 * Linux,保留 PC 分支语义:时基用 CLOCK_MONOTONIC;超时定时器为 no-op
 * (timeout 仅升级 level 不中断任务,该路径在板上验证,见 README)。
 */
#ifndef TASKER_PORT_H
#define TASKER_PORT_H

#include <stdint.h>

typedef void *tasker_timer_t;

#ifdef __cplusplus
extern "C" {
#endif

/* 毫秒时基(单调) */
uint64_t tasker_now_ms(void);

/* 一次性超时定时器(us 精度;Linux 移植返回 0 即成功但不启动真实定时器) */
int  tasker_timer_create(tasker_timer_t *timer, void (*cb)(void *), void *arg);
int  tasker_timer_start_once(tasker_timer_t timer, uint64_t us);
void tasker_timer_stop(tasker_timer_t timer);
void tasker_timer_delete(tasker_timer_t timer);

#ifdef __cplusplus
}
#endif

#endif /* TASKER_PORT_H */
