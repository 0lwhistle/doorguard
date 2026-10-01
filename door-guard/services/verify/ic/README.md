# services/verify/ic — IC 读卡业务层

> 2026-10-01 应用层落地。契约唯一事实源:`docs/tech/ICCARD_PROTOCOL.md`
> (驱动作者与应用层的双向接口,改任何一侧先改文档)。
> 硬件状态:驱动 .ko 与 DTS 未上线——provider 停在降级态退避重试
> (`/dev/dg_iccard0` open 失败),整机不受影响(协议 §10 失败隔离)。

## 文件

| 文件 | 职责 |
|---|---|
| `card_provider.h/.c` | 读卡线程:poll → read → 帧校验 → HEX 卡号 → `EV_IC_CARD`;降级广播;FLUSH 控制 |
| (`drv/iccard`) | 节点薄封装与帧契约,见 `drv/iccard/README.md` |

## 事件契约

| 事件 | 方向 | 载荷 |
|---|---|---|
| `EV_IC_CARD` | provider → access/enroll | `ev_ic_card_t`(卡号 HEX 串,大写无分隔) |
| `EV_ICCARD_CTRL` | enroll → provider | FLUSH(换会话清驱动帧缓冲;fd 由线程独占,这里置请求标志) |
| `EV_SYS_SERVICE_STATE("iccard")` | provider → UI/FSM | ERROR/READY 降级 latch(对齐 relay) |

provider **不做业务分流**:每帧原样发布;普通开门分支的防重窗(同一卡号
`door_open_ms` 内只算一次)在 access_service 按 FSM 状态执行——v_ic/录入
分支天然单发,不受窗限制(协议 §7.1 原文语义)。

## FSM 分支(§7.2 表,实现在 auth_fsm.c `FSM_EV_IC_CARD`)

- ST_NORMAL:黑名单 → reason=2 / 未开 IC → reason=6 / 陌生卡 → reason=1 /
  命中 → 开门,**method=4**;待机中刷卡先唤醒再走本分支。
- ST_ADMIN_AUTH:管理员卡进菜单(不开门);其余弹窗+日志停留本模式。
- ST_VERIFY `v_ic`:1:1 比对在 access_service(查库比卡号,同密码模式),
  经 `FSM_EV_VERIFY_RESULT` 进统一结果处理。
- 其余状态(弹窗非 v_ic/菜单/编辑页):忽略,不落日志。

## 使用示例

```c
/* 装配(app/main.c) */
registry_register("iccard", mod_iccard, false, DEP_CONFIG, 1, NULL, NULL);

/* 绑卡(UI → bridge → enroll:下一张刷入的卡生效) */
bridge_enroll_request(uid, DG_ENROLL_IC);
bridge_enroll_request(uid, DG_ENROLL_IC_CLEAR);   /* 解绑 */
/* 页面退出时 */ bridge_enroll_request(uid, DG_ENROLL_IC_CANCEL);

/* 宿主测试:sim 后端注入(drv/iccard/iccard_hal_sim.c,iccard.dev_path="sim") */
iccard_sim_inject((uint8_t *)"\x04\xA3\xB2\xC1", 4);
```

## 测试

- `test_iccard_proto`:帧校验/HEX/掩码 + sim pipe 语义。
- `test_card_dedup`:防重窗三则(窗内忽略/窗外放行/异卡不互斥)。
- `test_fsm_ic`:§7.2 表逐行 + reason=9 门禁。
- `test_enroll_ic`:绑卡端到端(成功/他人卡/幂等重绑/取消+FLUSH/解绑)。
- 板上验收(驱动就绪后):真卡开门、按住只开一次、重复卡录入被拒、拔模块降级提示。
