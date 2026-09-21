/*
 * dg_avatar.h — 用户头像显示控件(uid → DB JPEG → lv_img_dsc_t)
 *
 * LVGL 的 PNG/SJPG 解码器与文件系统适配在固件里全是关的(lv_conf 裁剪),
 * 所以走「libjpeg 解码成原始像素 + LV_IMG_CF_TRUE_COLOR」这条路——不需要
 * 任何解码器扩展,lv_img_set_src 直接吃内存位图。
 *
 * 用法(列表缩略图 / 编辑页预览各取所需):
 *   const lv_img_dsc_t *dsc = dg_avatar_get(uid, DG_AVATAR_THUMB);
 *   if (dsc) lv_list_add_row(list, dsc, text, cb);   // 无头像传 NULL = 不显示
 *
 * 数据变化(录入/清除/删除)后先 dg_avatar_invalidate(uid) 再重新 get。
 */
#ifndef DG_WIDGETS_AVATAR_H
#define DG_WIDGETS_AVATAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 两种固定出图规格(头像库统一 160×160,缩略图用 libjpeg 1/4 缩放解码) */
typedef enum {
    DG_AVATAR_FULL = 0,   /**< 160×160:编辑页预览 / 拍摄页回看 */
    DG_AVATAR_THUMB,      /**< 40×40:用户管理列表缩略图 */
} dg_avatar_size_t;

/**
 * 取 uid 头像的显示描述符(LVGL 线程调用;内部缓存,重复调用近零开销)。
 * @return NULL = 无头像或解码失败(调用方显示占位「无」)。
 * 返回的 dsc 归本控件所有,同 (uid,size) 期间稳定;数据变化后调
 * dg_avatar_invalidate,旧指针即作废——页面刷新时重新 get 即可。
 */
const lv_img_dsc_t *dg_avatar_get(const char *uid, dg_avatar_size_t size);

/** 头像数据变化后失效缓存(录入成功 / 清除人脸 / 删除用户;NULL = 全部) */
void dg_avatar_invalidate(const char *uid);

#ifdef __cplusplus
}
#endif

#endif /* DG_WIDGETS_AVATAR_H */
