/*
 * dg_edit_nav.h — 编辑页统一导航(2026-09-27 用户反馈:保存与返回分离)
 *
 * 约定:返回按钮固定左上角,保存按钮(若页有保存概念)固定右上角;
 * 返回时经 is_dirty() 检测未保存修改,有则弹「保存退出 / 直接退出」确认,
 * 无(或保存后)直接返回——防止用户误触丢配置,也免去保存后多余的询问。
 */
#ifndef DG_EDIT_NAV_H
#define DG_EDIT_NAV_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** 未保存修改检测(NULL = 本页无 dirty 概念,返回不询问) */
    bool (*is_dirty)(void);
    /** 保存动作(与右上保存按钮同一动作);返回 true=成功,dg_edit_nav
     *  随即执行返回;false=留在页面(校验失败等)。「保存退出」复用它 */
    bool (*save)(void);
} dg_edit_nav_ops_t;

/**
 * 创建左上角返回按钮;with_save=true 时同时创建右上角保存按钮。
 * ops 可为 NULL(纯浏览页:只有返回,不询问)。
 */
void dg_edit_nav_create(lv_obj_t *parent, const dg_edit_nav_ops_t *ops,
                        bool with_save);

#ifdef __cplusplus
}
#endif

#endif /* DG_EDIT_NAV_H */
