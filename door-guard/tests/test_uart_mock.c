/*
 * test_uart_mock.c — uart_hal 回环后端测试(Phase 8)
 *
 * 验证串口框架:打开/发送/接收回调/关闭;真实帧协议待硬件手册,
 * 不在本测试范围(mock 回环 = 框架自身正确性)。
 */
#include "dg_test.h"
#include "uart_hal.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static atomic_int s_rx_cnt = 0;
static uint8_t s_rx_buf[256];
static size_t s_rx_len = 0;

static void on_rx(const uint8_t *data, size_t len, void *ud)
{
    (void)ud;
    memcpy(s_rx_buf, data, len < sizeof(s_rx_buf) ? len : sizeof(s_rx_buf));
    s_rx_len = len;
    atomic_fetch_add(&s_rx_cnt, 1);
}

int main(void)
{
    uart_config_t cfg = { .device = "mock", .baud = 115200,
                          .data_bits = 8, .parity = 'N', .stop_bits = 1 };
    DG_CHECK(uart_hal_open(&cfg, on_rx, NULL) == DG_OK);
    DG_CHECK(uart_hal_is_open());
    DG_CHECK(uart_hal_open(&cfg, on_rx, NULL) == DG_ERR_BUSY);  /* 重复打开拒绝 */

    /* 回环:发送即接收 */
    const uint8_t frame[] = { 0xA5, 0x01, 0x00, 0x5A };
    for (int round = 0; round < 3; round++) {
        DG_CHECK(uart_hal_send(frame, sizeof(frame)) == DG_OK);
        for (int i = 0; i < 100 && atomic_load(&s_rx_cnt) <= round; i++)
            usleep(5000);
        DG_CHECK(atomic_load(&s_rx_cnt) == round + 1);
        DG_CHECK(s_rx_len == sizeof(frame));
        DG_CHECK(memcmp(s_rx_buf, frame, sizeof(frame)) == 0);
    }
    printf("[UART] mock loopback 3 frames OK\n");

    /* 参数错误显式拒绝 */
    DG_CHECK(uart_hal_send(NULL, 0) == DG_ERR_PARAM);

    uart_hal_close();
    DG_CHECK(!uart_hal_is_open());
    DG_CHECK(uart_hal_send(frame, sizeof(frame)) == DG_ERR_PARAM);  /* 关后拒发 */

    DG_TEST_EXIT();
}
