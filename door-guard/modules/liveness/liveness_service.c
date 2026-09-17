/* liveness_service.c — 活体占位实现(接口见 liveness_service.h) */
#include "liveness_service.h"
#include "dg_log.h"

int liveness_service_start(void)
{
    DG_LOGI("[LIVENESS]", "活体服务占位(未启用)");
    return DG_OK;
}

dg_liveness_result_t liveness_check(void)
{
    return DG_LIVE_SKIP;
}
