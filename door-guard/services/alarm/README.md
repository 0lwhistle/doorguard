# alarm — 远程报警上报(services/alarm)

> 2026-10-05 落地(接口先行)。触发源(防拆/胁迫/门磁)硬件与业务尚未
> 接入;本服务先把「报警怎么出去」收口——所有报警统一走
> `<prefix>/event/alarm` 主题,平台侧(Home Assistant 等)订阅联动。

## 职责与边界

- **只管出口**:类型/详情 → JSON → mqtt_publish_json(邮箱投递,宁丢不堵)
  + 本地日志留痕(DG_LOGW,报警线索不依赖通道在线)。
- **不管触发**:谁报、何时报属业务层。接入新触发源 = include 头文件 +
  调 `alarm_report()`,不再各自拼主题/字段。

## 主题与载荷

`<p>/event/alarm`(retain=false——报警走实时通知,离线补看日志):

```json
{"type":"tamper","code":0,"detail":"back cover opened","ts":1759900000}
```

| type | code | 触发源状态 |
|---|---|---|
| tamper | 0 | 防拆(待硬件) |
| forced_open | 1 | 强行开门(待门磁) |
| duress | 2 | 胁迫开门(待业务) |
| offline | 3 | 长期离线(平台侧判定,设备难自证) |
| custom | 4 | 自定义(detail 承载) |

detail 过长截断,引号/反斜杠净化(与 mqtt_service 同口径,不破 JSON 结构)。

## API

```c
#include "alarm/alarm_service.h"

alarm_report(ALARM_TAMPER, "back cover opened");   /* 任意线程 */
/* 返回:DG_OK=已投递;DG_ERR_NOT_INIT=mqtt 未启用(本地日志仍写) */
```

测试注入口:`alarm_sink_set(fn)` 替换发送 sink(`tests/test_alarm.c`
用注入捕获断言字段口径;NULL 恢复默认 mqtt sink)。

## 测试

`tests/test_alarm.c`(宿主):字段口径/空详情省略/净化/返回值透传/
超长截断/默认 sink 未启用透传。

```bash
./build-tests/test_alarm
```
