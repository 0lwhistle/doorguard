/*
 * page_home.h — 主页视图接口(pages 层;业务事件渲染在 presenter_home)
 */
#ifndef DG_PAGE_HOME_H
#define DG_PAGE_HOME_H

#include "lvgl.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

void page_home_create(lv_obj_t *parent);
void page_home_destroy(void);

/** 脸框渲染(state:DG_BOX_*;坐标为屏像素) */
void page_home_set_facebox(int state, int32_t x, int32_t y, int32_t w, int32_t h);
/** 只改脸框颜色(服务层命中/失败重新着色;位置沿用视觉后端逐帧刷新的检测框) */
void page_home_set_facebox_color(int state);
void page_home_clear_facebox(void);

/** 提示条(text 已翻译;NULL=隐藏) */
void page_home_set_hint(const char *text);

/** 相机可用态(2026-09-30 断流检测):不可用=提示条「摄像头未就绪」+ 清脸框
 *  + 预览盖半透明白幕;恢复=撤白幕(提示条若仍是相机文案则一并清) */
void page_home_set_cam_ready(bool ready);

/* ---- 设备故障提示(2026-09-30 holder 健康管理推广;四类共用一条提示条,
 *      优先级 门锁 > 相机 > 人脸 > 存储,恢复自动让位;只在无故障且当前
 *      文案是故障文案时才清,不抢验证流程的引导提示) ---- */

/** 人脸识别后端可用(看门狗禁用 rknn worker 时不可用) */
void page_home_set_face_ready(bool ready);
/** 开门继电器健康(脉冲写失败=验证成功但门未动作) */
void page_home_set_relay_ok(bool ok);
/** 存储健康(SELECT 1 巡检失败=记录可能丢失) */
void page_home_set_storage_ok(bool ok);


#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_HOME_H */
