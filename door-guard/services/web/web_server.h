/*
 * web_server.h — 内嵌 web 上位机(spec-network §1,mongoose 统一事件循环)
 *
 * 传输层跑在 modules/net/netcore 的 loop 线程上(2026-09-22 起替代 civetweb)。
 * 端口:device_config web_port(默认 8080)。
 * 安全(逐条对应实现):
 *   - 除静态页(/ 与 /assets 资源)与登录外,全部要求 token
 *     (X-Auth-Token;WebSocket 因浏览器无法自定义头,用 ?token=)
 *   - 口令 PBKDF2 存储(web_auth),token 表见 web_session
 *   - 登录失败按来源计数锁定(web_auth 风控),防局域网脚本爆破
 *   - 改凭据即吊销全部会话;单会话策略:同一时刻仅一个管理员在线
 */
#ifndef DG_WEB_SERVER_H
#define DG_WEB_SERVER_H

#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int web_server_start(void);
void web_server_stop(void);

/** 最近活动心跳(unix ms;透传 netcore 事件循环线程心跳)。
 * 看门狗判活用(registry 心跳钩子);未启动返回 0(看门狗按"无数据"跳过) */
int64_t web_server_heartbeat_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_WEB_SERVER_H */
