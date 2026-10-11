/*
 * fp_sync.c — 指纹两侧数量一致性:纯判定 + 状态/告知存储
 * 规则与边界见 fp_sync.h 头注释。本文件无 IO、无事件依赖,纯宿主可测。
 */
#include "fp_sync.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static fp_sync_status_t s_status;      /* 零初始化 = checked=false */
static int32_t s_notice;               /* 0 = 无待告知 */

/* 告知优先级:动作类最高,其后不一致,读失败最低(纯信息) */
static int notice_rank(int32_t n)
{
    switch (n) {
    case FP_NOTICE_CLEARED_MODULE:
    case FP_NOTICE_CLEARED_DB:
        return 3;
    case FP_NOTICE_MISMATCH:
        return 2;
    case FP_NOTICE_READ_FAIL:
        return 1;
    default:
        return 0;
    }
}

fp_sync_verdict_t fp_sync_decide(uint32_t n_db, uint32_t n_mod)
{
    /* 先判"两边都为 0":全新设备/双双清空,绝不能落进下面的单边分支 */
    if (n_db == 0 && n_mod == 0)
        return FP_SYNC_OK;
    if (n_mod == 0)                    /* n_db > 0 */
        return FP_SYNC_CLEAR_DB;
    if (n_db == 0)                     /* n_mod > 0 */
        return FP_SYNC_CLEAR_MODULE;
    if (n_db != n_mod)
        return FP_SYNC_MISMATCH;
    return FP_SYNC_OK;                 /* 数量相同:视为一致 */
}

void fp_sync_status(fp_sync_status_t *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&s_mtx);
    *out = s_status;
    pthread_mutex_unlock(&s_mtx);
}

bool fp_sync_take_notice(int32_t *out_notice)
{
    bool have = false;
    pthread_mutex_lock(&s_mtx);
    if (s_notice != FP_NOTICE_NONE) {
        if (out_notice)
            *out_notice = s_notice;
        s_notice = FP_NOTICE_NONE;
        have = true;
    }
    pthread_mutex_unlock(&s_mtx);
    return have;
}

void fp_sync_publish(uint32_t n_db, uint32_t n_mod, int32_t verdict,
                     int32_t err, int64_t at_ms)
{
    pthread_mutex_lock(&s_mtx);
    s_status.checked = true;
    s_status.n_db = n_db;
    s_status.n_mod = n_mod;
    s_status.verdict = verdict;
    s_status.err = err;
    s_status.at_ms = at_ms;
    pthread_mutex_unlock(&s_mtx);
}

void fp_sync_push_notice(int32_t notice)
{
    pthread_mutex_lock(&s_mtx);
    /* 高优先级不被顶掉;同级(含重复)后者覆盖,便于"最后一次动作"为准 */
    if (notice_rank(notice) >= notice_rank(s_notice))
        s_notice = notice;
    pthread_mutex_unlock(&s_mtx);
}
