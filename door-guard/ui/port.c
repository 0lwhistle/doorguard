/*
 * port.c — UI 平台端口实现
 */
#include "port.h"

#include <time.h>

uint32_t dg_ui_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
