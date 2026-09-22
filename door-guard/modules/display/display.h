/*
 * display.h — 显示 HAL(LVGL 显示驱动 + 触摸输入注册)
 *
 * PC 模拟器:SDL2 窗口 720×1280(DG_SIM 构建);
 * 板上:DRM/LVGL(Phase 8 接入,当前为占位)。
 * 同一份 ui/ 代码跑两处(spec-ui),业务与 UI 层不感知后端差异。
 */
#ifndef DG_DISPLAY_H
#define DG_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化显示后端并注册 LVGL 驱动;须在 lv_init() 之后调用 */
int display_init(void);

/** 事件泵(SIM:SDL 事件→LVGL 输入;板:空实现)——主循环每帧调用 */
void display_poll(void);

/** 触摸活动监听:每次「按下沿」(手指落下瞬间)回调一次,UI 用来把
 *  「有操作」喂给服务层(EV_UI_TOUCH:清待机/菜单无操作计数)。
 *  注意是按下沿而非每次采样,否则 LVGL 30Hz 轮询会刷爆事件总线 */
void display_set_touch_listener(void (*fn)(void));

/** 后端内部用:按下沿通知(touch_evdev/display_sim 的 read_cb 调用) */
void display_touch_activity(void);

/* ---- 视频 overlay plane(video-plane 直通预览,2026-09-22) ----
 * 板上把 NV12 dma-buf 直接送 VOP2 硬件扫描输出(zpos 压到 UI plane 之下),
 * 预览零 CPU;sim 恒 false。show 失败自动置不可用,页面据此走软渲染回退 */

/** 硬件 plane 直通是否可用(首次 show 前调用有效;sim 恒 false) */
bool display_has_video_plane(void);

/** 显示一帧 NV12(dmabuf_fd 由相机持有,slot 为稳定槽位键做 fb 缓存);
 *  返回 DG_OK / DG_ERR_*(失败即永久降级) */
int display_video_plane_show(int slot, int dmabuf_fd, int32_t w, int32_t h,
                             int32_t stride);

/** 停止视频 plane 显示(离开预览页时调用) */
void display_video_plane_hide(void);

/** 双 dumb fb 清 0(ARGB 下=全透明)。透明底页面(主页)创建时先调,
 *  清掉上一页不透明残留;之后 partial 刷新只画控件,未画区保持透明 */
void display_clear_fbs(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_DISPLAY_H */
