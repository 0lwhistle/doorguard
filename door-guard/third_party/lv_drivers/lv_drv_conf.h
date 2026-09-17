/*
 * lv_drv_conf.h — 仅为 lv_drivers/display/drm.c 提供配置(最小化)
 */
#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H

#define USE_DRM 1
#define DRM_CARD "/dev/dri/card0"
#define DRM_CONNECTOR_ID -1   /* -1 自动选第一个可用连接器 */

#endif
