/*
 * tasker_port.h — tasker 平台移植层(内部头)
 *
 * 移植自模板 ovs/components/core/tasker/tasker_port.h;door-guard 只跑
 * Linux:时基用 CLOCK_MONOTONIC。模板的超时定时器在 Linux 移植里恒为
 * no-op(超时只升级 level、不中断任务,业务从不依赖)——整段已删
 * (2026-10-25 B6-5);业务超时由状态机 timer_seq 机制承担。
 */
#ifndef TASKER_PORT_H
#define TASKER_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 毫秒时基(单调) */
uint64_t tasker_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* TASKER_PORT_H */
