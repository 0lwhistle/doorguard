# proto/ — 契约层(模块间通信的唯一语言)

模块间只经 proto/ 的消息与 event_bus 通信,禁止跨层直调。本目录三类契约:

| 头文件 | 内容 | 消费方 |
|---|---|---|
| `err.h` | 统一错误码(0 成功/负数失败,分段:通用 -1~-19、用户 -20~-29、验证 -30~-39、网络 -40~-49) | 全部模块;UI 按此映射提示文案 |
| `types.h` | `user_rec_t` / `access_log_t` / `log_query_t` / `log_page_t`,users/access_logs 表的唯一内存投影,字段对齐 spec-database §1/§4;含 role/auth_flags 位宽静态断言 | storage/enroll/access/UI |
| `events.h` | `EV_*` 业务事件全集 + 负载结构(编译期守卫 ≤256B);`dg_event_name()` 事件名 | vision/access/enroll/net/HAL/UI |

基础组件:`event_bus/`(发布订阅)、`tasker/`(短任务调度)、`holder/`(模块注册表),
各有独立 README。`dg_log.h` 为共用极简日志。

## 使用示例

```c
#include "events.h"
#include "err.h"

/* 发布认证结果( access_service ) */
ev_auth_result_t ev = { .has_user = true, .method = DG_METHOD_FACE_1N,
                        .result = DG_RESULT_PASS, .reason = DG_REASON_OK,
                        .ts = time(NULL) };
snprintf(ev.user_id, sizeof(ev.user_id), "%s", rec->user_id);
EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &ev);

/* 订阅并渲染弹窗(UI 层,不做业务决策) */
event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, page_ctx);

/* 错误码直传:storage 的返回码进事件负载,UI 只翻译不编码 */
int rc = db_user_add(&rec);          /* rc = DG_ERR_DUP_FACE(-23) */
```

## 测试

`tests/test_proto.c`:事件契约(18 种事件发布→订阅回捕,负载逐字段相等)、
EV_AUTH_RESULT 显式字段比对、事件名/错误码名全覆盖、位宽静态断言编译期生效。
