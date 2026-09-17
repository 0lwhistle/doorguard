/*
 * display_drm.c — 板上显示后端(占位)
 *
 * 待硬件确认(DEVLOG 登记):
 * - 屏幕点亮走 SDK LVGL demo 的 DRM/KMS 路径(B6 阶段定位节点:IMX415 与
 *   屏幕的 plane 分配、rknpu2 demo 的显示初始化代码)
 * - GT9xx 触摸输入事件节点(/dev/input/event1 待实测确认)
 * Phase 8 完成 DRM 平面接入(视频层)与 LVGL UI 层合成。
 */
#include "display.h"
#include "dg_log.h"

int display_init(void)
{
    DG_LOGW("[DISPLAY]", "DRM 后端 Phase 8 接入,当前板上无 UI 显示");
    return DG_ERR_NOT_INIT;
}

void display_poll(void)
{
}
