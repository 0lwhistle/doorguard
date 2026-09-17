/*
 * access_service.h — 认证融合/门控/日志唯一出口(spec-database §4、architecture §1)
 *
 * 持有验证状态机(auth_fsm):订阅视觉事件与 UI 请求(EV_UI_*),
 * 执行 FSM 动作——落库、开门(EV_AUTH_DOOR_OPEN)、结果广播(EV_AUTH_RESULT,
 * 字段与 access_logs 一致)、切页/提示(EV_UI_GOTO_PAGE / EV_UI_HINT)。
 * UI 只渲染,不决策。
 */
#ifndef DG_ACCESS_SERVICE_H
#define DG_ACCESS_SERVICE_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动(依赖:storage/cfg/event_bus/tasker);注册 1s 心跳(tasker) */
int access_service_start(void);

/** 停止(测试用) */
void access_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_ACCESS_SERVICE_H */
