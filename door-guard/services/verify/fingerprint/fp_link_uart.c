/*
 * fp_link_uart.c — fp_provider 板级链路(uart_hal 收发 + gpio_hal WAK 边沿)
 *
 * FINGERPRINT_AS608 §1:内核侧零驱动,纯应用层 UART;WAK 是"指纹按下"的
 * 物理事件源,采集流程靠它,不轮询模组。uart_hal 是全局单例(当前唯一
 * 用户即本文件);接收线程字节经环形缓冲交 provider 的命令-应答引擎。
 */
#include "cfg.h"
#include "dg_log.h"
#include "err.h"
#include "fp_provider.h"
#include "gpio_hal.h"
#include "uart_hal.h"
#include "timeutil.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- 接收环形缓冲(uart rx 线程 → provider 线程) ---- */

#define LINK_RING_SZ 2048
static pthread_mutex_t s_ring_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_ring[LINK_RING_SZ];
static size_t s_ring_len;

static void on_uart_rx(const uint8_t *data, size_t len, void *ud)
{
    (void)ud;
    fprintf(stderr, "[DBG] uart rx %zuB b0=%02x\n", len, data[0]);
    pthread_mutex_lock(&s_ring_mtx);
    if (len > sizeof(s_ring) - s_ring_len)
        len = sizeof(s_ring) - s_ring_len;
    memcpy(s_ring + s_ring_len, data, len);
    s_ring_len += len;
    pthread_mutex_unlock(&s_ring_mtx);
}

static int link_open(void)
{
    uart_config_t c = {
        .device = cfg_get()->fp_uart_dev,
        .baud = cfg_get()->fp_baud,
        .data_bits = 8,
        .parity = 'N',
        .stop_bits = 1,
    };
    pthread_mutex_lock(&s_ring_mtx);
    s_ring_len = 0;                       /* 新链路不背旧字节 */
    pthread_mutex_unlock(&s_ring_mtx);
    return uart_hal_open(&c, on_uart_rx, NULL);
}

static void link_close(void)
{
    uart_hal_close();
    pthread_mutex_lock(&s_ring_mtx);
    s_ring_len = 0;
    pthread_mutex_unlock(&s_ring_mtx);
}

static int link_send(const uint8_t *data, size_t len)
{
    return uart_hal_send(data, len);
}

static int link_recv(uint8_t *buf, size_t cap, int timeout_ms, size_t *out_len)
{
    *out_len = 0;
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&s_ring_mtx);
        size_t n = s_ring_len < cap ? s_ring_len : cap;
        if (n) {
            memcpy(buf, s_ring, n);
            memmove(s_ring, s_ring + n, s_ring_len - n);
            s_ring_len -= n;
        }
        pthread_mutex_unlock(&s_ring_mtx);
        if (n) {
            *out_len = n;
            return 0;
        }
        if (now_ms() >= deadline)
            return DG_ERR_TIMEOUT;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

static int link_wak_wait(int timeout_ms, int *level)
{
    int line = cfg_get()->fp_wak_gpio;
    if (line <= 0)
        return DG_ERR_UNSUPPORTED;        /* WAK 未接线:事件驱动不可用 */
    return gpio_hal_edge_wait(line, timeout_ms, level);
}

static int link_wak_level(int *level)
{
    int line = cfg_get()->fp_wak_gpio;
    if (line <= 0)
        return DG_ERR_UNSUPPORTED;
    return gpio_hal_in_level(line, level);
}

const fp_link_ops_t fp_link_uart = {
    .open = link_open,
    .close = link_close,
    .send = link_send,
    .recv = link_recv,
    .wak_wait = link_wak_wait,
    .wak_level = link_wak_level,
};
