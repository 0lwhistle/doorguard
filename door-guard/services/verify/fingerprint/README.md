# services/verify/fingerprint — AS608 指纹业务层

> 2026-10-01 应用层落地。协议唯一事实源:`docs/tech/FINGERPRINT_PROTOCOL.md`(v1 冻结);
> 业务方案与决策:`docs/tech/FINGERPRINT_AS608.md`(决策 A/B/C 已拍板)。
> 硬件状态:模组已接线(uart8/57600、WAK=GPIO2_D6/94),**uart8 时钟修复(DTS 24MHz)
> 后方可真机联调**——未修复前 provider 停在降级态退避重试,整机不受影响。

## 文件

| 文件 | 职责 |
|---|---|
| `fp_as608.h/.c` | 协议层(纯函数):组包/流式解析/校验和/确认码。不持线程不碰 IO,golden 单测 `test_fp_proto` |
| `fp_provider.h/.c` | 业务线程:WAK 按压沿 → 采集/比对/录入序列 → 发事件;降级与 PageID 映射 |
| `fp_link_uart.c` | 板级链路 ops:uart_hal 收发(uart 单例)+ gpio_hal WAK 边沿等待 |

## 事件契约(proto/events.h)

| 事件 | 方向 | 载荷 |
|---|---|---|
| `EV_FINGER_SET_MODE` | access/enroll → provider | `ev_finger_mode_t`(IDLE/SCAN_1N/VERIFY_11/ENROLL/FINGER_DEL/DELETE_USER) |
| `EV_FINGER_STATUS` | provider → 任意 | PRESSED/RELEASED/ERROR(按压沿) |
| `EV_FINGER_MATCH_1N` | provider → access | `ev_match_t`(含 auth_flags,黑名单/方式位过滤归 FSM,method=2) |
| `EV_FINGER_VERIFY_11` | provider → access | `ev_match_t`(v_finger 子步 1:1) |
| `EV_ENROLL_PROGRESS` / `EV_ENROLL_RESULT` | provider → UI | 两次按压进度(step 语义见 `dg_enroll_fp_step_t`)与终态 |

provider 忙于录入/删除序列时忽略 IDLE/SCAN/VERIFY 常规模式;**取消 = 切 IDLE**,
未落库模板自动 `DeletChar` 回滚。

## 降级(模组没接/串口不通 ≠ 系统坏)

open/握手(VerifyPSW)连续失败 ≥3 → `EV_SYS_SERVICE_STATE("finger", ERROR)` 一次,
此后 2s 退避静默重试;恢复自动回 READY。access_service 据此喂 FSM `finger_ready`,
验证方式选择时选指纹立即 reason=9(UI 文案「指纹模块未就绪」),不空等超时。

## 使用示例

```c
/* 装配(app/main.c):注册为可选服务,线程循环即心跳 */
registry_register("finger", mod_finger, false, DEP_CONFIG, 1,
                  fp_provider_heartbeat_ms, NULL);

/* 录入(UI → bridge → enroll → provider,seq 全程配对) */
bridge_enroll_request(uid, DG_ENROLL_FINGER);          /* 两次按压 */
bridge_enroll_request(uid, DG_ENROLL_FINGER_DEL);      /* arg = page_id */
bridge_enroll_request(uid, DG_ENROLL_FINGER_CANCEL);   /* 退出页面时撤销 */

/* 宿主测试:假模组注入(tests/test_fp_enroll.c) */
fp_provider_set_link_ops(&FAKE_OPS);   /* 须在 start 前 */
fp_provider_start();
```

## 测试

- `test_fp_proto`:协议 golden(官方 51 例程逐字节)。
- `test_fp_enroll`:假模组端到端——happy path/查重/LIMIT/FULL/两次不一致重采/取消回滚/逐枚删除。
- 板上验收清单:`FINGERPRINT_PROTOCOL.md §6`(uart8 时钟修复后逐条勾销)。
