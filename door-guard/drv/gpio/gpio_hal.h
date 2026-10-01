/*
 * gpio_hal.h — 门控 GPIO(开门继电器)
 *
 * 实现:B4 rootfs 无 libgpiod/gpiod CLI(实测),采用 sysfs 接口
 * (/sys/class/gpio/export + gpioN/value);rootfs 升级带 libgpiod 后迁移。
 * 引脚号进 default.json(access.relay_gpio_line,出厂占位 0,
 * 待硬件确认:继电器实际接线引脚,DEVLOG 登记)。
 * 语义:开门 = 拉高 door_open_ms 后拉低(电平型继电器假设;
 * 脉冲型继电器待硬件确认后调整)。
 */
#ifndef DG_GPIO_HAL_H
#define DG_GPIO_HAL_H

#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化:导出引脚并设为输出(已导出/被内核占用返回显式错误) */
int gpio_hal_init(int line_no);

/** 开门:拉高 ms 毫秒后拉低(阻塞调用,ms ≤ 10000) */
int gpio_hal_door_pulse(uint32_t ms);

/** 电平读取(对拍/自检:读 /sys/class/gpio/gpioN/value) */
int gpio_hal_get_level(int *level);

/** 电平直设(0/1;安全停机把继电器复位到断开态用,不走开门脉冲) */
int gpio_hal_set_level(int level);

/** 读回引脚号(-1 未初始化) */
int gpio_hal_line(void);

/* ---- 输入引脚边沿等待(FINGERPRINT_AS608:指纹 WAK 触摸事件源) ---- */

/**
 * 等待输入引脚电平跳变(sysfs edge=both + poll POLLPRI;阻塞至多 timeout_ms)。
 * @param level 出参:跳变后的电平(0/1)
 * @return DG_OK 有边沿 / DG_ERR_TIMEOUT 超时 / DG_ERR_IO 引脚不可用
 * @note 首次调用做 export+direction=in+edge=both(幂等);引脚被内核占用
 *       返回 DG_ERR_IO。当前设计同一时刻只有一个输入引脚用户(指纹 WAK),
 *       换引脚号自动重建;多输入引脚并存待需要时扩展。
 */
int gpio_hal_edge_wait(int line_no, int timeout_ms, int *level);

/** 解除阻塞中的 gpio_hal_edge_wait(停服务用;等待方收到 DG_ERR_IO) */
int gpio_hal_edge_abort(void);

/** 指定输入引脚电平直读(消抖复核用;与继电器单例无关) */
int gpio_hal_in_level(int line_no, int *level);

void gpio_hal_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_GPIO_HAL_H */
