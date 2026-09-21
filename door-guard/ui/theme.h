/*
 * theme.h — 蓝白主题 token(色值唯一来源,spec-ui §1)
 *
 * 全项目只引用 token 不写裸色值;改动须经 spec-ui 确认。
 */
#ifndef DG_THEME_H
#define DG_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- spec-ui §1 色值 ---- */
#define DG_COLOR_PRIMARY        0x1E88E5 /**< 主蓝:按钮底/标题栏/选中态 */
#define DG_COLOR_PRIMARY_DARK   0x1565C0 /**< 按钮按下态 */
#define DG_COLOR_BG             0xFFFFFF /**< 页面底色 */
#define DG_COLOR_BG_LIGHT       0xE3F2FD /**< 卡片/分区底 */
#define DG_COLOR_TEXT           0x212121 /**< 主文字 */
#define DG_COLOR_OK             0x2E7D32 /**< 成功(绿) */
#define DG_COLOR_ERR            0xC62828 /**< 失败(红) */
#define DG_COLOR_WARN           0xF9A825 /**< 脸框黄(检测中) */
#define DG_COLOR_SCRIM          0x000000 /**< 推流/照片上的文字衬底(半透明黑) */

#define DG_COLOR_PRIM(c)  lv_color_hex(DG_COLOR_PRIMARY)
#define DG_COLOR_DARKC(c) lv_color_hex(DG_COLOR_PRIMARY_DARK)
#define DG_COL_BG()       lv_color_hex(DG_COLOR_BG)
#define DG_COL_BG_LIGHT() lv_color_hex(DG_COLOR_BG_LIGHT)
#define DG_COL_TEXT()     lv_color_hex(DG_COLOR_TEXT)
#define DG_COL_OK()       lv_color_hex(DG_COLOR_OK)
#define DG_COL_ERR()      lv_color_hex(DG_COLOR_ERR)
#define DG_COL_WARN()     lv_color_hex(DG_COLOR_WARN)
#define DG_COL_SCRIM()    lv_color_hex(DG_COLOR_SCRIM)

/* ---- 层次感 token(2026-09-22 用户反馈:控件层级靠透明度/暗化/半透明白边)
 * 用法:卡片与浮层 = DG_COL_BG_LIGHT 底 + 白色半透明描边;推流上的文字 =
 * 黑色半透明衬底 chip;次要文字(行标题/"无"占位)降透明度 */
#define DG_OPA_SCRIM        LV_OPA_40  /**< 推流上 chip 衬底透明度 */
#define DG_OPA_CARD_LINE    LV_OPA_60  /**< 卡片白边透明度 */
#define DG_OPA_TEXT_DIM     LV_OPA_50  /**< 次要文字透明度 */

/* ---- 版式(720×1280 竖屏) ---- */
#define DG_SCREEN_W   720
#define DG_SCREEN_H   1280
#define DG_BTN_H      96   /**< 主按钮高度(触摸友好) */
#define DG_RADIUS     12
#define DG_PAD        16

/** 中文界面字体(由 ui/font 生成,覆盖 lang 目录 json 全部字符) */
LV_FONT_DECLARE(dg_font_cn_16);
#define DG_FONT_CN &dg_font_cn_16

#ifdef __cplusplus
}
#endif

#endif /* DG_THEME_H */
