# mqtt — MQTT 上位机通道

> 2026-10-04 落地(任务一)。设备 ↔ 平台的消息干道,跑在 netcore 统一事件循环
> (mongoose `mg_mqtt`,与 web/NTP/mDNS 同一传输层),默认**关闭**。

## 职责与边界

- **本服务只做通道**:连接保活/退避重连、命令订阅与应答、事件上报转发;
  业务语义(远程开门怎么走权限与留痕)不在本层,见「扩展接口」。
- 线程契约:所有 `mg_*` 只在 netcore loop 线程;业务线程经邮箱投递发布请求
  (满丢最旧计数——遥测宁丢不堵,绝不反阻塞总线)。

## 配置(default.json `mqtt` 段;json-only,改动重启生效)

| 键 | 默认 | 说明 |
|---|---|---|
| `enabled` | `false` | 整链路开关 |
| `uri` | `""` | broker,如 `mqtt://192.168.137.1:1883` |
| `client_id` | `""` | 空 = `doorguard-<主接口IP 末段>` |
| `topic_prefix` | `doorguard` | 主题前缀 |
| `username` / `password` | `""` | broker 认证,可空 |
| `allow_remote_open` | `false` | `cmd/open` 授权开关(安全默认拒) |

## 主题方案(`<p>` = topic_prefix)

| 主题 | 方向 | 说明 |
|---|---|---|
| `<p>/status` | 设备→平台 | retain;上线 `{"state":"online"}`,掉线由 LWT 自动落 `offline` |
| `<p>/hello` | 设备→平台 | 连上即发 `{"ip":"..."}` |
| `<p>/event/auth` | 设备→平台 | 每次验证动作(与 access_logs 同口径;未连接不发) |
| `<p>/event/service` | 设备→平台 | 看门狗处置/服务降级 |
| `<p>/cmd/+` | 平台→设备 | 命令入口(QoS1);内置 `ping` / `status` / `open` |
| `<p>/rsp/<name>` | 设备→平台 | 应答 `{"ok":true,...}` 或 `{"ok":false,"err":"..."}` |

## 扩展接口(后续功能开发挂载点)

```c
#include "mqtt/mqtt_service.h"

/* 1. 任意业务上报(任意线程,内部邮箱投递) */
mqtt_publish_json("event/door", "{\"n\":1}", false);

/* 2. 注册命令:平台发 <p>/cmd/snap 即触发,应答自动回 <p>/rsp/snap */
static int cmd_snap(const char *payload, char *resp, size_t cap) {
    snprintf(resp, cap, "\"sn\":\"%s\"", cfg_get()->face_model_tag);
    return DG_OK;
}
mqtt_cmd_register("snap", cmd_snap);   /* 启动期调用;在 loop 线程执行,禁阻塞 */

/* 3. 事件式消费(不便注册回调时):订阅 proto EV_MQTT_CMD */
```

`cmd/open` 特殊:默认拒绝;`allow_remote_open=true` 时也只发布 EV_MQTT_CMD,
不在本层直碰 relay——开门必须走 access 流程留 access_logs,等消费端接入
(接入后同步更新本表与 events.h 注释)。

## 健壮性

- 断线重连:5/10/20/60s 指数退避,连上即复位;CONNACK 拒绝同样走退避。
- 半开检测:keepalive 30s,半周期 PINGREQ,3×keepalive 无任何入包强制重连。
- 看门狗:registry 心跳 = netcore loop 活性;未启用返回 0(不判失联)。

## 测试

`tests/test_mqtt.c`:测试内起最小 broker(CONNACK/SUBACK/PUBACK/PINGRESP),
覆盖未启用空转、握手+订阅+retain 上线、ping 往返、未知命令、自定义命令、
业务上报、验证事件转发(含用户名引号净化)、停服 offline 告别。宿主直跑:

```bash
./build-tests/test_mqtt
```
