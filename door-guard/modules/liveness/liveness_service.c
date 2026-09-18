/*
 * liveness_service.c — 动作活体实现(B7:留口;B8:动作状态机)
 *
 * B7 只做三件事:记账最近一次人脸关键点/质量、按 cfg 决定是否告警、
 * 门禁恒放行(算法未实现,block 会把门禁变成"永远不开门")。算法本身
 * 属 B8(见 liveness_service.h 文件头与 docs/tech/B7_FACE_HANDOFF.md §0.1)。
 */
#include "liveness_service.h"
#include "cfg.h"
#include "dg_log.h"

#include <pthread.h>
#include <time.h>

static const char *TAG = "[LIVENESS]";

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool     has;                        /* 有过一次回灌 */
    int64_t  ms;                         /* 最近回灌时刻(单调) */
    uint32_t n;                          /* 点数 */
    int32_t  quality;                    /* 质量分 */
} s_last;
static bool s_warned;                    /* "已启用但未实现"只告警一次 */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int liveness_service_start(void)
{
    DG_LOGI(TAG, "活体服务就绪(cfg liveness_enable=%d;算法 B8 实现)",
            cfg_get()->liveness_enable);
    return DG_OK;
}

int liveness_service_on_face(const dg_face_pt_t *pts, uint32_t n, int32_t quality)
{
    if (n > DG_LANDMARK_MAX)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    s_last.has = true;
    s_last.ms = now_ms();
    s_last.n = pts ? n : 0;
    s_last.quality = quality;
    pthread_mutex_unlock(&s_mtx);

    /* B8 在此处喂动作状态机(眨眼/点头/摇头);B7 只记账 */
    return DG_OK;
}

bool liveness_service_pass(void)
{
    if (!cfg_get()->liveness_enable) {
        /* 未启用:放行(与 DG_LIVE_SKIP 同义) */
        return true;
    }
    if (!s_warned) {
        s_warned = true;
        DG_LOGW(TAG, "liveness_enable=1 但算法未实现(B8):本次放行,不阻断验证");
    }
    return true;                         /* B8:返回动作序列判定结果 */
}

dg_liveness_result_t liveness_check(void)
{
    return liveness_service_pass() ? DG_LIVE_PASS : DG_LIVE_PENDING;
}

int64_t liveness_service_last_face_age_ms(void)
{
    int64_t ms;
    bool has;
    pthread_mutex_lock(&s_mtx);
    has = s_last.has;
    ms = s_last.ms;
    pthread_mutex_unlock(&s_mtx);
    return has ? now_ms() - ms : -1;
}
