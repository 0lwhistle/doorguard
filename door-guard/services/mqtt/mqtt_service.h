/*
 * mqtt_service.h — MQTT 上位机通道(services/mqtt)
 *
 * 定位:设备 ↔ 平台的消息干道,跑在 netcore 统一事件循环(与 web/NTP/mDNS
 * 同一传输层,mongoose mg_mqtt)。默认关闭(cfg mqtt.enabled),broker 地址
 * 属部署参数;开启后自动重连、掉线遗嘱(LWT)、验证事件上报、远程命令订阅。
 *
 * 主题方案(<prefix> = cfg mqtt.topic_prefix,默认 doorguard):
 *   <prefix>/status        retain,设备在线状态("online"/"offline" LWT)
 *   <prefix>/event/auth    每次验证动作(成功/失败,JSON)
 *   <prefix>/event/service 看门狗处置/服务降级(JSON)
 *   <prefix>/cmd/+         订阅的远程命令(内置 ping/status;open 需授权)
 *   <prefix>/rsp/<name>    命令应答(JSON:{"ok":...} 或 {"err":"..."})
 *
 * 扩展接口(后续功能开发的挂载点,2026-10-04):
 *   1. mqtt_publish_json(suffix, json, retain) —— 任意业务侧上报;
 *   2. mqtt_cmd_register(name, fn) —— 注册 <prefix>/cmd/<name> 处理器,
 *      fn 在 netcore loop 线程执行,返回 dg_err_t,应答自动回 rsp/<name>;
 *   3. EV_MQTT_CMD(proto/events.h)—— 不便注册回调的场景按事件消费。
 *
 * 线程契约:所有 mg_* 调用都在 netcore loop 线程(经 netcore_post 进入);
 * 业务线程只调本头文件 API(内部邮箱投递,永不阻塞)。
 */
#ifndef DG_MQTT_SERVICE_H
#define DG_MQTT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动服务。cfg mqtt.enabled=0 → 空转(记一条日志即返回,不建连接);
 *  幂等。netcore 须已启动(依赖声明在装配层)。 */
int mqtt_service_start(void);

/** 停服:断开连接、撤重连定时器、注销总线订阅。幂等。 */
void mqtt_service_stop(void);

/** broker 是否已连接(CONNACK 通过且未断开) */
bool mqtt_service_connected(void);

/** 业务上报:<prefix>/<suffix> 主题发 JSON 载荷(任意线程,邮箱投递,
 *  满即丢最旧并计数——遥测类消息宁丢不堵)。未启用/未连接时返回
 *  DG_ERR_NOT_INIT 由调用方决定是否提示(验证事件转发内部已容错)。 */
int mqtt_publish_json(const char *suffix, const char *json, bool retain);

/** 远程命令处理器:payload = 命令载荷原文(可为空串);resp 写应答 JSON
 *  (空串 = 只回 ok/err 骨架);返回 DG_OK → rsp {"ok":true,...}。
 *  在 netcore loop 线程执行,禁止阻塞(要慢操作请自行投递)。 */
typedef int (*mqtt_cmd_fn)(const char *payload, char *resp, size_t resp_cap);

/** 注册 <prefix>/cmd/<name> 命令处理器(启动期调用;name ≤15 字符,
 *  上限 8 个,满/重名返回 DG_ERR_PARAM) */
int mqtt_cmd_register(const char *name, mqtt_cmd_fn fn);

/** 看门狗心跳(loop 活性;未启用返回 0 = 不判失联) */
int64_t mqtt_service_heartbeat_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_MQTT_SERVICE_H */
