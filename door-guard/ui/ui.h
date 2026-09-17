/*
 * ui.h — UI 子系统引导(spec-ui:同一份 UI 代码跑 PC 模拟器与板上)
 */
#ifndef DG_UI_H
#define DG_UI_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *lang_dir;      /**< 语言表目录(ui/lang) */
    const char *camera_dir;    /**< 相机 sim 图片目录(SIM 构建) */
} dg_ui_args_t;

/** LVGL + 显示 + 输入 + 语言表 + 页面管理器;并注册/打开各页面 */
int ui_init(const dg_ui_args_t *args);

/** 主循环每帧调用(display 事件泵 + lv_timer_handler) */
void ui_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_UI_H */
