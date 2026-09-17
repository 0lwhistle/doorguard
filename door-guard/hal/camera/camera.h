/*
 * camera.h — 相机 HAL(RGB888 帧投递)
 *
 * 帧只经本接口流动,UI/服务不感知来源:PC=sim 图片/视频循环,
 * 板=V4L2/ISP 真实流(Phase 8)。回调在 camera_poll 调用线程执行,
 * 消费方只读,回调返回后缓冲失效(零拷贝,消费方需保留则自行拷贝)。
 */
#ifndef DG_CAMERA_H
#define DG_CAMERA_H

#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t w, h;              /**< 帧宽高(RGB888) */
    uint32_t seq;              /**< 递增序号(丢帧检测) */
    const uint8_t *pixels;     /**< RGB888,回调返回后失效 */
} camera_frame_t;

typedef void (*camera_frame_fn)(const camera_frame_t *frame, void *ud);

/**
 * 初始化。
 * SIM 后端:res_path = 图片目录(循环播放,支持 png/jpg/bmp)。
 * 板上后端:res_path = 设备节点(Phase 8)。
 */
int camera_init(const char *res_path, camera_frame_fn cb, void *ud);

/** 投帧泵(主循环调用;SIM 按帧率节拍投递) */
void camera_poll(void);

/** sim 后端帧率(ms/帧) */
void camera_sim_set_interval(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* DG_CAMERA_H */
