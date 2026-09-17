/*
 * web_server.h — 内嵌 web 上位机(spec-network §1,civetweb 单库)
 *
 * 端口:device_config web_port(默认 8080)。
 * 功能:登录(token)/ WebSocket 实时事件 / 日志查询 / 设备信息+NTP /
 *       OTA 上传端点 / 视频页(占位)。
 * 安全:除登录与静态页外全部校验 X-Auth-Token;口令 PBKDF2 存储。
 */
#ifndef DG_WEB_SERVER_H
#define DG_WEB_SERVER_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int web_server_start(void);
void web_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_WEB_SERVER_H */
