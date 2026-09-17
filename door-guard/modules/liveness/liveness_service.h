/*
 * liveness_service.h — 动作活体(预留占位;PROJECT_PLAN §二)
 *
 * 单目 RGB 动作指令式:随机动作序列(眨眼/张嘴/摇头)+ 姿态判定。
 * 接口占位:Phase 8+ 按需求实现;当前 liveness_check 恒返回"跳过"
 * (验证流程不受阻),不发布事件。
 */
#ifndef DG_LIVENESS_SERVICE_H
#define DG_LIVENESS_SERVICE_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int liveness_service_start(void);

typedef enum {
    DG_LIVE_SKIP = 0,     /**< 未启用,直接放行 */
    DG_LIVE_PASS,
    DG_LIVE_FAIL,
    DG_LIVE_PENDING,
} dg_liveness_result_t;

dg_liveness_result_t liveness_check(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_LIVENESS_SERVICE_H */
