/*
 * ui_features.h — 设备端 UI 功能可见性开关
 *
 * 2026-10-19 IC 卡 UI 全屏蔽:读卡链路挂起期间,设备端不出现任何 IC 入口
 * (用户编辑行、验证方式勾选项、验证方式选择)。开关与业务侧同源——
 * `iccard.enabled` 已让 main/access 停线程停订阅,UI 跟同一配置走,
 * 恢复时改配置重启即可,不需要再改代码(与 web 端 IC_UI_ENABLED 对应)。
 */
#ifndef DG_UI_FEATURES_H
#define DG_UI_FEATURES_H

#include <stdbool.h>

#include "cfg.h"

/** IC 卡相关 UI 是否可见(绑定/解绑行、验证方式选项、「请刷卡」提示) */
static inline bool dg_ui_ic_enabled(void)
{
    return cfg_get()->iccard_enabled != 0;
}

#endif /* DG_UI_FEATURES_H */
