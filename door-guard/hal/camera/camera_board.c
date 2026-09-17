/*
 * camera_board.c — 板上相机后端(占位)
 *
 * 待硬件确认(DEVLOG 登记):IMX415 真实链路 = rkaiq 3A + V4L2/多媒体
 * 取流(B6/B7 阶段打通,与 ROCKIVA 对接的 NV12 环形缓冲;UI 预览层的
 * RGB 转换点在 RGA)。Phase 8 完成接入;当前返回未初始化,业务层据
 * EV_CAPTURE_STATE 显示"摄像头未就绪"(spec-auth-business §5)。
 */
#include "camera.h"

#include <stddef.h>
#include "dg_log.h"

#include <dirent.h>
#include <string.h>

static const char *TAG = "[CAMERA]";

int camera_init(const char *res_path, camera_frame_fn cb, void *ud)
{
    (void)res_path;
    (void)cb;
    (void)ud;
    /* 实测线索记录:列出 /dev/video* 供 B6/B7 ISP 链路定位 */
    DIR *d = opendir("/dev");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL)
            if (strncmp(ent->d_name, "video", 5) == 0)
                DG_LOGI(TAG, "发现节点 /dev/%s(B6/B7 链路候选)", ent->d_name);
        closedir(d);
    }
    DG_LOGW("[CAMERA]", "真实链路(rkaiq+V4L2)待 B6/B7,当前 mock 占位");
    return DG_ERR_NOT_INIT;
}

void camera_poll(void)
{
}

const camera_frame_t *camera_latest(void)
{
    return NULL;
}
