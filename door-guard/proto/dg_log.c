/*
 * dg_log 实现 — 见 dg_log.h
 */
#include "dg_log.h"

#include <stdarg.h>
#include <stdio.h>

static dg_log_level_t s_level = DG_LOG_INFO;

void dg_log_set_level(dg_log_level_t level)
{
    s_level = level;
}

dg_log_level_t dg_log_get_level(void)
{
    return s_level;
}

static const char *level_char(dg_log_level_t level)
{
    switch (level) {
    case DG_LOG_DEBUG: return "D";
    case DG_LOG_INFO:  return "I";
    case DG_LOG_WARN:  return "W";
    case DG_LOG_ERROR: return "E";
    default:           return "?";
    }
}

void dg_log_write(dg_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (level < s_level)
        return;

    va_list ap;
    va_start(ap, fmt);
    /* stderr 无缓冲行模式,单次 vfprintf 原子性足够;不取时间戳:
     * 组件测试在毫秒级断言时序,日志自身耗时须最小化 */
    flockfile(stderr);
    fprintf(stderr, "[%s]%s ", level_char(level), tag ? tag : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(ap);
}
