/*
 * uart_hal.h — 串口框架(指纹模块 / IC 读卡器)
 *
 * 协议纪律:指纹/读卡的具体帧协议**等硬件手册,不臆造**(任务清单§5)。
 * 本 HAL 只提供:termios 打开与参数配置、收发字节流、后台接收线程;
 * mock 后端(回环)供宿主 ctest 验证框架本身。
 * 协议解析层待手册到位后在 auth/{finger,card} 实现,本 HAL 不涉及。
 */
#ifndef DG_UART_HAL_H
#define DG_UART_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device;       /**< 设备节点(/dev/ttyS*);"mock" = 回环后端 */
    int baud;
    uint8_t data_bits;
    char parity;              /* 'N'/'E'/'O' */
    uint8_t stop_bits;
} uart_config_t;

typedef void (*uart_rx_fn)(const uint8_t *data, size_t len, void *ud);

/** 打开并启动接收线程(mock 后端 device=="mock":发什么回什么) */
int uart_hal_open(const uart_config_t *cfg, uart_rx_fn rx, void *ud);

/** 发送(线程安全) */
int uart_hal_send(const uint8_t *data, size_t len);

/** 停止接收线程并关闭 */
void uart_hal_close(void);

bool uart_hal_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_UART_HAL_H */
