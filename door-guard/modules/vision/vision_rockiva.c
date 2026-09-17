/*
 * vision_rockiva.c — 板上 ROCKIVA 视觉后端(占位)
 *
 * 待硬件确认(DEVLOG 登记):B7 阶段 ROCKIVA 上板(NPU 驱动/rkaiq 确认后),
 * 接入点:capture NV12 帧 → rockiva_face_detect/recognize → 特征经
 * vision_service_submit_feature 入槽 → 1:N 检索走库内特征比较
 * (storage_set_feature_cmp 注入 ROCKIVA 相似度比较器)。
 * 当前返回未初始化;板上业务不受阻(mock 路径可演示,DEVLOG 已标注)。
 */
#include "vision_service.h"
#include "dg_log.h"

void vision_backend_start(bool enable_mock)
{
    (void)enable_mock;
    /* 板上:录入抓取订阅可用(mock 特征,演示闭环);
     * 真实检测/检索待 B7/B8 ROCKIVA 接入 */
    DG_LOGW("[VISION]", "ROCKIVA 后端 B7/B8 接入;当前仅录入抓取可用");
}
