/*
 * gpio_hal.c — sysfs GPIO 实现(接口见 gpio_hal.h)
 *
 * 错误显式处理:export/direction/value 每步失败都返回 DG_ERR_IO,
 * 不静默;EBUSY(引脚被内核占用)单独区分。
 */
#include "gpio_hal.h"
#include "dg_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "[GPIO]";

static int s_line = -1;

static int write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        DG_LOGE(TAG, "open %s: %s", path, strerror(errno));
        return (errno == EBUSY) ? DG_ERR_BUSY : DG_ERR_IO;
    }
    size_t len = strlen(val);
    ssize_t w = write(fd, val, len);
    close(fd);
    if (w != (ssize_t)len) {
        DG_LOGE(TAG, "write %s=%s: %s", path, val, strerror(errno));
        return (errno == EBUSY) ? DG_ERR_BUSY : DG_ERR_IO;
    }
    return DG_OK;
}

static int read_sysfs_int(const char *path, int *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return DG_ERR_IO;
    char buf[16] = { 0 };
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return DG_ERR_IO;
    *out = atoi(buf);
    return DG_OK;
}

int gpio_hal_init(int line_no)
{
    if (s_line == line_no)
        return DG_OK;                       /* 幂等 */
    if (line_no < 0)
        return DG_ERR_PARAM;

    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/export");
    char val[16];
    snprintf(val, sizeof(val), "%d", line_no);
    write_sysfs(path, val);                 /* 已导出会 EBUSY,幂等继续 */

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", line_no);
    int rc = write_sysfs(path, "out");
    if (rc != DG_OK) {
        DG_LOGE(TAG, "gpio%d 设输出失败(引脚被占用?换 access.relay_gpio_line)",
                line_no);
        return rc;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", line_no);
    int lv = 0;
    if (read_sysfs_int(path, &lv) != DG_OK) {
        DG_LOGE(TAG, "gpio%d 不存在(导出失败)", line_no);
        return DG_ERR_IO;
    }

    s_line = line_no;
    DG_LOGI(TAG, "gpio%d 就绪(当前电平 %d)", line_no, lv);
    return DG_OK;
}

int gpio_hal_door_pulse(uint32_t ms)
{
    if (s_line < 0)
        return DG_ERR_NOT_INIT;
    if (ms == 0 || ms > 10000)
        return DG_ERR_PARAM;

    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", s_line);
    int rc = write_sysfs(path, "1");
    if (rc != DG_OK)
        return rc;

    usleep((useconds_t)ms * 1000);

    rc = write_sysfs(path, "0");
    if (rc != DG_OK)
        return rc;
    DG_LOGI(TAG, "开门脉冲 %ums 完成", ms);
    return DG_OK;
}

int gpio_hal_get_level(int *level)
{
    if (s_line < 0 || !level)
        return DG_ERR_PARAM;
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", s_line);
    return read_sysfs_int(path, level);
}

int gpio_hal_set_level(int level)
{
    if (s_line < 0)
        return DG_ERR_NOT_INIT;
    if (level != 0 && level != 1)
        return DG_ERR_PARAM;
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", s_line);
    return write_sysfs(path, level ? "1" : "0");
}

int gpio_hal_line(void)
{
    return s_line;
}

void gpio_hal_deinit(void)
{
    if (s_line >= 0) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/gpio/unexport");
        char val[16];
        snprintf(val, sizeof(val), "%d", s_line);
        if (write_sysfs(path, val) != DG_OK)
            DG_LOGW(TAG, "unexport gpio%d 失败", s_line);
        s_line = -1;
    }
}
