/*
 * liveness_service.h — 动作活体(单目 RGB 动作指令式;PROJECT_PLAN §4.4)
 *
 * 数据流(B8 已实装):
 *   vision 后端(逐帧)──liveness_service_on_face(5 关键点)──▶ 本模块状态机
 *   本模块 ──EV_LIVENESS_PROMPT──▶ UI 顶层提示 + access(FSM 判定窗/步计时)
 *   本模块 ──liveness_service_pass()──▶ vision 命中发布前的门禁
 *
 * 门禁语义(活体恒启用,开关 2026-10-04 删除):
 *   - 人脸在场即随机出 2~3 步动作(点头/摇头),每步限时(cfg),全过才
 *     pass()==true,vision 的命中才允许发布(FSM 的 1.5s 判定窗也由
 *     PASSED 事件武装——见 auth_fsm.c);
 *   - 超时/未按要求 = 本步作废、重出新序列(照片挡门只会被反复出题,
 *     不产生失败弹窗/日志——判定窗没开,FSM 看不到命中);
 *   - 关键点非 5 点的后端(ROCKIVA 106 点,布局未映射)降级放行并告警,
 *     与单帧反欺骗「模型缺失自动放行」同一取舍(见 README)。
 * 动作判定全用同轴比例(yaw 用 x/x、点头用 y/y),与帧宽高比无关。
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

/* ---- 关键点回灌与命中门禁 ----
 * (旧接口 liveness_check()/dg_liveness_result_t 已删:B7 改口后零调用者,
 *  2026-09-28;命中门禁走下方 pass(),结果语义 bool) */

/** 人脸关键点(与后端解耦的归一坐标;ROCKIVA 给万分比 0~9999) */
typedef struct {
    int32_t x, y;         /**< 万分比坐标(0~9999),与分辨率无关 */
} dg_face_pt_t;

#define DG_LANDMARK_MAX 106

/* rknn 主线的 5 点布局(万分之一):0 左眼 / 1 右眼 / 2 鼻尖 / 3 左嘴角 /
 * 4 右嘴角(RetinaFace 标准序)。本模块的几何判定按这个序取点。 */
#define DG_LIVE_KPS 5

/**
 * 视觉后端每帧(检出人脸)回灌关键点。
 * @param pts     关键点数组(万分比);可为 NULL(仅回灌质量)
 * @param n       点数(>DG_LANDMARK_MAX 截断;n != 5 走降级判定,见文件头)
 * @param quality 人脸质量分(0~100;rknn 主线暂传 0,几何判定不用质量)
 * @return DG_OK / DG_ERR_PARAM
 */
int liveness_service_on_face(const dg_face_pt_t *pts, uint32_t n, int32_t quality);

/**
 * 命中门禁:true = 允许发布/放行。
 * 仅当「当前在场的动作序列已全过」(或非 5 点后端降级)时为 true。
 */
bool liveness_service_pass(void);

/** 最近一次回灌的年龄(ms;无回灌返回 -1)——联调观测与陈旧保护用 */
int64_t liveness_service_last_face_age_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_LIVENESS_SERVICE_H */
