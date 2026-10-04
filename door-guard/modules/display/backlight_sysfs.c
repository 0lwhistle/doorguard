/*
 * backlight_sysfs.c — 背光亮度(sysfs;PWM 背光标准接口,屏 dtsi 已点亮)
 *
 * 单独成文件的原因:display 后端有四个变体(板/PC × LVGL9/8.3),
 * 背光与 LVGL 渲染无关,一份实现四处挂接(dg_display 各变体统一编译本文件)。
 *
 * LVGL 线程(page_standby 生命周期)与 event_bus 分发线程(bridge 订阅
 * 回调)都会调用,互斥保护探测与写入。brightness fd 常开,重复写为一次
 * sysfs store(内核侧毫秒级)。路径取 /sys/class/backlight 下第一个条目
 * (板上 kickpi K7 仅一个背光设备);PC/无节点环境 = WARN 一次后恒失败,
 * 调用方不依赖返回值即可安全降级。
 */
#include "display.h"
#include "dg_log.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static int s_bl_fd = -2;        /* -2=未探测, -1=探测过但无节点, >=0 常开 fd */
static int s_bl_max = 1;
static int s_bl_warned;
static pthread_mutex_t s_bl_mu = PTHREAD_MUTEX_INITIALIZER;

static void backlight_probe(void)
{
    DIR *d = opendir("/sys/class/backlight");
    if (!d) {
        s_bl_fd = -1;
        return;
    }
    struct dirent *e;
    /* "/sys/class/backlight/" + d_name(≤255) + "/max_brightness" 截断分析咬住 */
    char path[320];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        FILE *f;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness",
                 e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        int max = -1;
        if (fscanf(f, "%d", &max) == 1 && max > 0) {
            fclose(f);
            snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
                     e->d_name);
            int fd = open(path, O_WRONLY);
            if (fd >= 0) {
                s_bl_fd = fd;
                s_bl_max = max;
                DG_LOGI("[DISPLAY]", "背光就绪 %s max=%d", path, max);
                closedir(d);
                return;
            }
        } else {
            fclose(f);
        }
    }
    closedir(d);
    s_bl_fd = -1;
}

int display_backlight_set(int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    pthread_mutex_lock(&s_bl_mu);
    if (s_bl_fd == -2)
        backlight_probe();
    if (s_bl_fd < 0) {
        if (!s_bl_warned) {
            s_bl_warned = 1;
            DG_LOGW("[DISPLAY]", "无背光节点,亮度控制不可用(设置被忽略)");
        }
        pthread_mutex_unlock(&s_bl_mu);
        return DG_ERR_IO;
    }
    int val = (int)((long long)s_bl_max * pct / 100);
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", val);
    int rc = DG_OK;
    if (write(s_bl_fd, buf, (size_t)n) < 0)
        rc = DG_ERR_IO;
    pthread_mutex_unlock(&s_bl_mu);
    return rc;
}
