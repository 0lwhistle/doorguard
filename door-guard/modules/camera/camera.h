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
    int32_t w, h;              /**< 帧宽高 */
    uint32_t seq;              /**< 递增序号(丢帧检测) */
    const uint8_t *pixels;     /**< XRGB8888(4 字节/像素,与 LVGL 32 位色直通),
                                    回调返回后失效 */
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

/** 最近一帧(渲染端 100ms 级轮询拷贝,帧不过事件总线);
 *  无帧/未就绪返回 NULL。帧内存由 camera 模块持有,下次 poll 前有效 */
const camera_frame_t *camera_latest(void);

/** sim 后端帧率(ms/帧) */
void camera_sim_set_interval(uint32_t ms);

/* ---- NV12 出口(板端视觉专用;sim 无实现) ----
 * on_frame 在 camera_poll 调用线程执行(板上=主循环:UI/渲染/触摸同线程)。
 * 硬契约:回调必须快、绝不阻塞——重活(推理/编码/写库)自行转线程,回调里
 * 只做投递;不处理的帧必须立刻 on_release(frame_id),否则 4 个 V4L2 缓冲
 * 耗尽,相机永久断流。 */
typedef void (*camera_nv12_fn)(const uint8_t *data, int w, int h,
                               uint32_t frame_id);
typedef void (*camera_nv12_release_fn)(uint32_t frame_id);
void camera_set_nv12_listener(camera_nv12_fn on_frame,
                              camera_nv12_release_fn on_release);
void camera_nv12_release(uint32_t frame_id);

#ifdef __cplusplus
}
#endif

#endif /* DG_CAMERA_H */
