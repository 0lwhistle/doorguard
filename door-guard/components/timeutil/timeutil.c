/*
 * timeutil.c — 公共时基实现(契约见 timeutil.h)
 */
#include "timeutil.h"

#include <time.h>

static int64_t gettime_ms(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_ms(void)
{
    return gettime_ms(CLOCK_REALTIME);
}

int64_t now_mono_ms(void)
{
    return gettime_ms(CLOCK_MONOTONIC);
}

int64_t now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}
