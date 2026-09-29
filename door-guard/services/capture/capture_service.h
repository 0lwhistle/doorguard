/*
 * capture_service.h — 取流状态服务(architecture §1)
 *
 * 1s 巡检相机流健康(camera_stream_on/camera_last_frame_ms 事实),断流
 * 判定后广播 EV_CAPTURE_STATE{ready} 并同步 holder "camera" 模块状态:
 *   - 未就绪/中途死机(含传感器须断电才救活的硬件级死亡)→ ready=false
 *   - UI 订阅后显示"摄像头未就绪"并把预览清为半透明白(spec-auth §5-113)
 *   - FSM 订阅后人脸 1:1 步直接 reason=9 失败,不空等 5s 超时
 * 状态变化才发布(避免事件刷屏);启动过渡期(流尚未起)不发事件,
 * capture_camera_ready() 以乐观值应答——正常开机 1~2s 内流即起来。
 */
#ifndef DG_CAPTURE_SERVICE_H
#define DG_CAPTURE_SERVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int capture_service_start(void);
void capture_service_stop(void);

/** 最近一次发布的相机可用态;从未发布过(启动过渡期)返回 true */
bool capture_camera_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_CAPTURE_SERVICE_H */
