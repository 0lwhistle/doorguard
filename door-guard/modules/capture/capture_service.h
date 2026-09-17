/*
 * capture_service.h — 取流服务(architecture §1)
 *
 * 相机帧经 camera HAL 的 latest 帧接口给 UI(100ms 轮询);本服务负责
 * 状态监测与广播(EV_CAPTURE_STATE):未就绪/断流时 UI 显示
 * "摄像头未就绪",验证路径 reason=9(spec-auth §5)。
 * 板上 V4L2/ISP 真实取流 Phase 8 接入,服务接口不变。
 */
#ifndef DG_CAPTURE_SERVICE_H
#define DG_CAPTURE_SERVICE_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int capture_service_start(void);
void capture_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_CAPTURE_SERVICE_H */
