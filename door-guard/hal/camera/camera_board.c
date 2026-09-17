/*
 * camera_board.c — 板上相机后端(占位)
 *
 * 待硬件确认(DEVLOG 登记):IMX415 真实链路 = rkaiq 3A + V4L2/多媒体
 * 取流(B6/B7 阶段打通,与 ROCKIVA 对接的 NV12 环形缓冲;UI 预览层的
 * RGB 转换点在 RGA)。Phase 8 完成接入;当前返回未初始化,业务层据
 * EV_CAPTURE_STATE 显示"摄像头未就绪"(spec-auth-business §5)。
 */
#include "camera.h"
#include "dg_log.h"

int camera_init(const char *res_path, camera_frame_fn cb, void *ud)
{
    (void)res_path;
    (void)cb;
    (void)ud;
    DG_LOGW("[CAMERA]", "板上后端 Phase 8 接入,当前无真实流");
    return DG_ERR_NOT_INIT;
}

void camera_poll(void)
{
}
