/*
 * uart_hal.c — 串口框架实现
 *
 * mock 后端:device=="mock" 时,send 的数据经接收回调原样回吐(回环),
 * 用于宿主 ctest 验证框架;真实串口:termios 原始模式 + 读线程。
 * 硬件手册到位后,帧协议解析在 auth/{finger,card} 层实现。
 */
#include "uart_hal.h"
#include "dg_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

static const char *TAG = "[UART]";

static int s_fd = -1;
static bool s_mock = false;
static volatile bool s_rx_run = false;
static pthread_t s_rx_tid;
static bool s_rx_created = false;
static uart_rx_fn s_rx_cb = NULL;
static void *s_rx_ud = NULL;

static void *rx_thread(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    while (s_rx_run) {
        ssize_t n = read(s_fd, buf, sizeof(buf));
        if (n > 0 && s_rx_cb)
            s_rx_cb(buf, (size_t)n, s_rx_ud);
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            break;
        usleep(10000);
    }
    return NULL;
}

int uart_hal_open(const uart_config_t *cfg, uart_rx_fn rx, void *ud)
{
    if (!cfg || !cfg->device || !rx)
        return DG_ERR_PARAM;
    if (s_fd >= 0)
        return DG_ERR_BUSY;

    s_rx_cb = rx;
    s_rx_ud = ud;
    s_mock = (strcmp(cfg->device, "mock") == 0);
    if (s_mock) {
        s_fd = 1000;                        /* 回环后端句柄占位 */
        s_rx_run = true;
        DG_LOGI(TAG, "mock 回环后端就绪");
        return DG_OK;
    }

    s_fd = open(cfg->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s_fd < 0) {
        DG_LOGE(TAG, "open %s: %s", cfg->device, strerror(errno));
        return DG_ERR_IO;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(s_fd, &tio) != 0) {
        close(s_fd);
        s_fd = -1;
        return DG_ERR_IO;
    }
    cfmakeraw(&tio);
    speed_t sp = B115200;
    switch (cfg->baud) {
    case 9600:  sp = B9600;  break;
    case 19200: sp = B19200; break;
    case 38400: sp = B38400; break;
    case 57600: sp = B57600; break;
    case 230400: sp = B230400; break;
    default:    sp = B115200; break;
    }
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(s_fd, TCSANOW, &tio) != 0) {
        close(s_fd);
        s_fd = -1;
        return DG_ERR_IO;
    }

    s_rx_run = true;
    if (pthread_create(&s_rx_tid, NULL, rx_thread, NULL) == 0)
        s_rx_created = true;
    DG_LOGI(TAG, "%s 就绪 %d 8%c%d", cfg->device, cfg->baud, cfg->parity,
            cfg->stop_bits);
    return DG_OK;
}

/* mock 回环:send 数据延迟后交接收回调(模拟响应时序) */
typedef struct {
    uint8_t *data;
    size_t len;
} mock_pkt_t;

static void *mock_echo(void *arg)
{
    mock_pkt_t *pkt = arg;
    usleep(5000);
    if (s_rx_cb)
        s_rx_cb(pkt->data, pkt->len, s_rx_ud);
    free(pkt->data);
    free(pkt);
    return NULL;
}

int uart_hal_send(const uint8_t *data, size_t len)
{
    if (s_fd < 0 || !data || len == 0)
        return DG_ERR_PARAM;

    if (s_mock) {
        mock_pkt_t *pkt = malloc(sizeof(*pkt));
        if (!pkt)
            return DG_ERR_NO_MEMORY;
        pkt->data = malloc(len);
        if (!pkt->data) {
            free(pkt);
            return DG_ERR_NO_MEMORY;
        }
        memcpy(pkt->data, data, len);
        pkt->len = len;
        pthread_t tid;
        if (pthread_create(&tid, NULL, mock_echo, pkt) != 0) {
            free(pkt->data);
            free(pkt);
            return DG_ERR_INTERNAL;
        }
        pthread_detach(tid);
        return DG_OK;
    }

    ssize_t w = write(s_fd, data, len);
    return (w == (ssize_t)len) ? DG_OK : DG_ERR_IO;
}

void uart_hal_close(void)
{
    s_rx_run = false;
    if (s_rx_created) {
        pthread_join(s_rx_tid, NULL);
        s_rx_created = false;
    }
    if (s_fd >= 0 && !s_mock)
        close(s_fd);
    s_fd = -1;
    s_mock = false;
}

bool uart_hal_is_open(void)
{
    return s_fd >= 0;
}
