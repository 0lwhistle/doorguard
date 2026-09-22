/*
 * camera.h — 相机 HAL(RGB888 帧投递)
 *
 * 帧只经本接口流动,UI/服务不感知来源:PC=sim 图片/视频循环,
 * 板=V4L2/ISP 真实流(Phase 8)。回调在 camera_poll 调用线程执行,
 * 消费方只读,回调返回后缓冲失效(零拷贝,消费方需保留则自行拷贝)。
 */
#ifndef DG_CAMERA_H
#define DG_CAMERA_H

#include <stdbool.h>
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

/** 预览旋转角(0/90/180/270,DG_CAM_ROT;视觉链路须把检测输入旋到同一
 * 方向,框/关键点才与预览同域——见 npu_pre_nv12_rotate)。未就绪时也给
 * 配置值:它来自环境变量,camera_init 时即定 */
int camera_rotation(void);

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

/* ---- NV12 dma-buf 出口(video plane 直通;sim 无实现) ----
 * 板上把「旋转后的 NV12」写入 dma-heap 缓冲,UI 取 fd 直送 VOP2 plane
 * 扫描输出(见 display.h)。槽位协议:最新帧经 latest_dmabuf 发布,UI
 * show 成功后 mark_shown(slot),相机写入时跳过正被扫描的槽(防撕裂)。 */

typedef struct {
    int fd;                    /**< NV12 dma-buf(UV 平面由 fb offset 表达) */
    int32_t w, h, stride;      /**< 旋转后尺寸(横装相机 90° → 720x1280) */
    uint32_t seq;              /**< 与 camera_frame_t 同源递增序号 */
    int slot;                  /**< 槽位键(0..VID_BUF_CNT-1) */
} camera_dmabuf_t;

/** 最近一帧已旋转 NV12(无则 NULL;指针指向模块静态存储,勿持有) */
const camera_dmabuf_t *camera_latest_dmabuf(void);

/** 标记正被 plane 扫描的槽位;slot<0 取消占用(hide/离开预览页) */
void camera_dmabuf_mark_shown(int slot);

/** LVGL 预览 RGB(XRGB)转换开关:plane 模式下关掉省一次 RGA;默认开 */
void camera_rgb_preview_set(bool on);

#ifdef __cplusplus
}
#endif

#endif /* DG_CAMERA_H */
