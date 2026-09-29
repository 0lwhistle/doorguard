/*
 * dg_preview.h — 相机预览控件(video plane 直通 / 软渲染双模)
 *
 * 板上(display_has_video_plane)自动走 VOP2 硬件 plane 直通:控件本体是
 * 透明占位,预览像素不经过 CPU;否则回退 lv_img 软渲染路径(抽点+zoom)。
 * 页面只需:create → 页面定时器 pump → destroy。sim 恒走软渲染路径。
 */
#ifndef DG_PREVIEW_H
#define DG_PREVIEW_H

#include "lvgl.h"

/** 创建预览控件;log_tag 用于节流日志前缀(如 "HOME"/"CAPTURE") */
lv_obj_t *dg_preview_create(lv_obj_t *parent, const char *log_tag);

/** 页面定时器回调:拉取最新帧并上屏(约 20ms 一次;内部有 seq 去重) */
void dg_preview_pump(lv_obj_t *p);

/** 相机可用态(2026-09-30 断流检测配套):
 *  false = 停泵,plane 模式隐藏 video plane(冻结帧不再扫描),控件上盖
 *          半透明白幕——最后一帧从「假直播」变「明确不可用」(spec-ui §3.1);
 *  true  = 撤白幕恢复泵,plane 模式下一新帧自动重新 show。
 *  可用态变化时调用即可;状态由 capture_service 经 UI_EVT_CAPTURE 推送 */
void dg_preview_set_available(lv_obj_t *p, bool avail);

/** 释放(hide plane / 归还槽位 / 释放缓冲) */
void dg_preview_destroy(lv_obj_t *p);

#endif /* DG_PREVIEW_H */
