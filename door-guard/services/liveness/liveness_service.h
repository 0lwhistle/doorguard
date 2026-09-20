/*
 * liveness_service.h — 动作活体(单目 RGB 动作指令式;PROJECT_PLAN §二)
 *
 * 数据流(B7 已通,B8 填算法):
 *   vision_rockiva(analyse 回调)──liveness_service_on_face(106 点)──▶ 本模块
 *   本模块 ──liveness_service_pass()──▶ vision 发布命中前的门禁
 *
 * B7 语义(留口,勿在 B8 之前改口):算法未实现 →
 *   - on_face 记账(最近一次关键点/质量),供 B8 状态机与调试;
 *   - pass() 恒 true(等价旧的 DG_LIVE_SKIP/未启用),不开门失败;
 *   - cfg liveness_enable=1 时打一条告警(明示"已启用但未实现,本次放行"),
 *     避免"以为开着活体其实没开"的静默失效。
 * B8 落地:随机 2~3 动作序列(眨眼 EAR 边沿/点头纵向位移比/摇头 yaw 往返),
 *   每步 5s 超时,全过置 pass;cfg liveness_actions_min/max/timeout_ms 生效。
 */
#ifndef DG_LIVENESS_SERVICE_H
#define DG_LIVENESS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

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

/* ---- B7 留口:关键点回灌与命中门禁 ---- */

/** 人脸关键点(与后端解耦的归一坐标;ROCKIVA 给万分比 0~9999) */
typedef struct {
    int32_t x, y;         /**< 万分比坐标(0~9999),与分辨率无关 */
} dg_face_pt_t;

#define DG_LANDMARK_MAX 106

/**
 * 视觉后端每帧(质量合格的人脸)回灌关键点。
 * @param pts     关键点数组(万分比);可为 NULL(仅回灌质量)
 * @param n       点数(>DG_LANDMARK_MAX 截断)
 * @param quality 人脸质量分(0~100;ROCKIVA faceQuality.score)
 * @return DG_OK / DG_ERR_PARAM
 */
int liveness_service_on_face(const dg_face_pt_t *pts, uint32_t n, int32_t quality);

/**
 * 命中门禁:true = 允许发布/放行。
 * B7 恒 true(算法未实现,见文件头);B8 返回动作序列判定结果。
 */
bool liveness_service_pass(void);

/** 最近一次回灌的年龄(ms;无回灌返回 -1)——B8 超时判定与联调观测用 */
int64_t liveness_service_last_face_age_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_LIVENESS_SERVICE_H */
