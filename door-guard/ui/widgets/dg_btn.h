/*
 * dg_btn.h — 统一按钮:图标 + label(spec-ui §1,所有按钮经此创建)
 */
#ifndef DG_WIDGETS_BTN_H
#define DG_WIDGETS_BTN_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建图标+文本按钮(蓝底白字,按下变深蓝;主题 token 见 theme.h)
 *  @param icon  LVGL 内置符号(LV_SYMBOL_*)或 NULL
 *  @param label 已过 _() 的文本
 */
lv_obj_t *dg_btn_create(lv_obj_t *parent, const char *icon, const char *label);

/** 蓝白反色变体(次级操作:浅蓝底深色字) */
lv_obj_t *dg_btn_create_light(lv_obj_t *parent, const char *icon, const char *label);

/** 改按钮文字(字母键盘大小写切换、页脚 ABC↔123 用;图标不动) */
void dg_btn_set_label(lv_obj_t *btn, const char *label);

#ifdef __cplusplus
}
#endif

#endif /* DG_WIDGETS_BTN_H */
