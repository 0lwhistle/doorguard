# IC 卡读卡器 驱动-应用层协议设计(SPI 读头接入)

> 面向:**驱动作者**(SPI 读卡器 Linux 驱动)与**应用层实现者**(door-guard 卡服务/FSM/UI)。
> 本文是双方的**接口契约**:驱动按 §2~§5 实现,应用层按 §2~§7 消费;任何一侧要改协议,
> 必须先改本文并双向确认,禁止单方面变更。
> 总体方案评审结论见会话记录(2026-09-27);业务细则以 `spec-auth-business.md`、
> `spec-database.md` 为权威,本文与其冲突时以 references 为准并回改本文。

## 1. 总体数据流与分层落点

```
ISO14443 卡片 → SPI 读卡芯片 → [Linux 驱动 /dev/dg_iccard0]      ← 本文 §2~§5,用户实现
    → [drv/iccard(iccard_hal)]    设备节点薄封装,无线程,含 sim 后端
    → [services/verify/ic/card_provider]  持读卡线程:poll/read → 卡号字符串
        ├─ 发 EV_IC_CARD(HAL 域,契约已冻结,负载 ev_ic_card_t)
        ├─ 同卡防重窗(door_open_ms)
        └─ 设备失联降级 EV_SYS_SERVICE_STATE
    → 消费者(只经事件总线,禁止直调):
        access/auth_fsm  ST_NORMAL/ST_ADMIN_AUTH 刷卡开门分支(§7.1)
        verify 编排      v_ic 子步 1:1(§7.1)
        enroll_service   录入态查重落库(§7.2)
```

既有契约盘点(**全部已预留,本次只填空**):`EV_IC_CARD`(`proto/events.h`,HAL 域 0x0002)、
`ev_ic_card_t { char card_no[DG_IC_LEN=32] }`、`auth_card_provider()` 与 `services/verify/ic/`
空目录、DB `ic_card TEXT UNIQUE` + `db_find_by_ic()` + `DG_ERR_DUP_IC`、日志 `method=4`、
FSM 的 `v_ic` 子步(spec-auth §4.3)。

## 2. 设备节点与文件操作语义(驱动契约)

| 项 | 约定 |
|---|---|
| 节点 | `/dev/dg_iccard0`(最终名由驱动定;应用侧经配置 `iccard.dev_path` 适配,见 §8) |
| 打开 | **独占**:已有读者时第二个 `open` 返回 `-EBUSY`(应用内只有一个消费者线程,独占防帧被分走) |
| 读模式 | 缺省阻塞;支持 `O_NONBLOCK`(无完整帧 → `-EAGAIN`) |
| `read()` | **一次消费一整帧**(24B,§3):缓冲 <24B 返回 `-EINVAL`;无帧时阻塞模式挂起 |
| `poll()` | `POLLIN` = 有完整帧可读;阻塞在 `poll/read` 的消费者在 `close(fd)` 时被唤醒返回 |
| `close()` | 唤醒所有阻塞的 `poll/read`;释放独占。**应用停线程 = 置停标志 + close(fd) + join**,不用 pthread_cancel |
| ioctl | `DG_ICCARD_IOC_FLUSH`(§4) |

## 3. 帧格式(应用层只见 UID,ISO14443 协议封装在驱动内)

驱动内完成 寻卡 → 防冲突 → 选卡 → 读 UID,`read()` 吐出的是最终 UID:

```c
#define DG_ICCARD_MAGIC    0x30434349UL   /* 'I','C','C','0' 小端排列 */
#define DG_ICCARD_UID_MAX  16

struct dg_iccard_frame {
    uint32_t magic;                  /* 恒为 DG_ICCARD_MAGIC,应用侧校验,不符整帧丢弃 */
    uint8_t  uid_len;                /* 合法 4~16,越界视为坏帧 */
    uint8_t  card_type;              /* 卡型,仅诊断不参与业务,见下 */
    uint16_t seq;                    /* 驱动内自 1 递增,回绕不处理;丢帧/重复诊断用 */
    uint8_t  uid[DG_ICCARD_UID_MAX]; /* UID 原样字节,不做任何字节序/反序变换 */
} __attribute__((packed));           /* 24 字节 */
```

`card_type` 建议枚举(值由驱动作者最终确认):`0=UNKNOWN 1=MF_CLASSIC 2=MF_ULTRALIGHT
3=NTAG 4=DESFIRE 5=FELICA 0xFF=OTHER`。

**UID 字节序钉死**:按芯片读出顺序原样存放,**不反序**。部分读头/行业习惯把 Mifare 4B UID
按小端显示,本工程不做——字节怎么读出来就怎么存,字符串化(§5)也按该顺序,全链路一致。

## 4. ioctl

```c
#define DG_ICCARD_IOC_MAGIC  'I'
#define DG_ICCARD_IOC_FLUSH  _IO (DG_ICCARD_IOC_MAGIC, 0x01)  /* 清空驱动内帧缓冲 */
#define DG_ICCARD_IOC_STATS  _IOR(DG_ICCARD_IOC_MAGIC, 0x02, struct dg_iccard_stats)

struct dg_iccard_stats {
    uint32_t rx_frames;    /* 完整帧累计 */
    uint32_t rx_overruns;  /* 环形缓冲满被丢弃的帧数 */
    uint32_t bad_frames;   /* magic/uid_len 非法被丢弃的帧数 */
    uint32_t irq_count;    /* 芯片中断累计(驱动走内部轮询实现时 = 寻卡周期数) */
};
```

`FLUSH` 的时点(应用层职责,驱动只需实现清空):**录入/验证模式切换时**、**页面退出录入态时**
必须调用,防止半秒前的旧卡串进新会话。

## 5. 卡号字符串化规则(DB/事件/日志/web 全链路口径,定后难改)

- **格式**:uid 按字节顺序逐字节**大写 HEX、无分隔符**。例:4B `04 A3 B2 C1` → `"04A3B2C1"`。
- 最短 8 字符(4B UID),最长 32 字符(16B UID),恰为 `DG_IC_LEN=32` 上限内。
- 转换只发生在 card_provider 一处(驱动吐原始字节,其余全链路只见字符串)。
- **显示掩码**(spec-database §3):`********` + 末 4 字符,如 `********B2C1`
  (HEX 卡号最短 8 字符,末 4 恒有;界面与日志一律掩码)。

## 6. 错误语义(驱动 → 应用)

| errno | 含义 | 应用侧动作 |
|---|---|---|
| `-EAGAIN` | 非阻塞模式无完整帧 | 正常,继续 poll |
| `-EIO` | 单次与芯片通信失败(寻卡/SPI 传输错) | 连续计数,达阈值进降级(§7.3) |
| `-ENODEV` | 设备已移除/芯片失联 | 进降级,退避重试 open |
| `-EBUSY` | 已有读者 | 不应出现(应用只有一个消费者),出现即记日志查因 |
| `-EINVAL` | read 缓冲 <24B | 编码错误,修复调用方 |

## 7. 应用层消费模型与业务集成

### 7.1 card_provider 线程(services/verify/ic/)

主循环:`poll(-1)` → `read()` → 校验 magic/uid_len → HEX 字符串化 → 发 `EV_IC_CARD`
(消费者按 FSM 状态自行分流,provider 不做业务判断)。

- **同卡防重窗**(普通开门分支专用):同一卡号在 `door_open_ms` 窗口内只算一次验证,
  窗内重复帧忽略——不弹窗、不落日志。不同卡号不受窗限制。v_ic 与录入分支天然单发
  (出结果/落库后子步即结束),不需要窗。
- **降级**:open 失败或 `-EIO` 连续 ≥5 次 → `EV_SYS_SERVICE_STATE`(读卡器未就绪),
  UI 对齐摄像头未就绪语义(reason=9 同款);退避 2s 重试,恢复后上报 READY。
- **停止**:置停标志 + `close(fd)` → `poll/read` 返回 → 线程退出。

### 7.2 FSM/业务分支(建议合入 spec-auth-business,下表为集成语义)

| FSM 状态收到 EV_IC_CARD | 处理 |
|---|---|
| ST_NORMAL | 普通刷卡验证:防重窗检查 → `db_find_by_ic` → 未开 IC(reason=6)/黑名单(2)/陌生卡(1)→ 红弹窗+日志;命中(role≠2 且 auth_flags bit3)→ 开门+日志,**method=4**。待机中刷卡先唤醒屏幕再走本分支 |
| ST_ADMIN_AUTH | 在 **role=1** 用户中查卡:命中管理员 → 进菜单(**不开门**,同人脸路径);否则红弹窗+日志,停留本模式继续尝试 |
| ST_VERIFY(非 v_ic 子步) | **忽略,不落日志**(弹窗操作中误碰他人卡不算验证动作) |
| ST_VERIFY `v_ic` | 与指定用户 1:1:卡号 == 该用户 `ic_card` → 成功开门+日志;≠ → 失败+日志(reason=4);出结果后子步结束,后续帧不再消费 |
| ST_MENU / 编辑页(非录卡态) | 忽略,不落日志 |
| 编辑页录卡态 | 不进 FSM,由 enroll 消费(§7.3) |

### 7.3 录入(enroll)

`EV_ENROLL_REQUEST(mode=IC)` → enroll 置录卡态(发起自编辑页,FSM 必在 ST_MENU,与
普通开门分支天然不冲突)→ 收到 EV_IC_CARD → 业务预检查重(**排除自身**,同指纹 0.75
那次的 update 语义)→ SQL `UNIQUE` 兜底 → 重复:`EV_ENROLL_RESULT(DG_ERR_DUP_IC)`
→ UI 红字「卡号重复,录入失败」;成功:落库 → RESULT 带卡号 → UI 按掩码显示。
页面退出/取消 → 撤销录卡态 + `FLUSH`。

### 7.4 auth_provider.h 修订(接入前待办)

该头文件是 v2 前草案:`user_id` 为 `uint32_t`,与现行 `proto/types.h` 字符串口径不一致;
其 `verify(timeout_ms)` 阻塞语义与 IC 的事件驱动模型不符(IC 实际编排走事件流,同人脸的
`EV_VISION_VERIFY_11` 模式)。实现 `auth_card_provider()` 前先按现行契约修订此头,
IC 的 provider 仅作登记与生命周期管理,认证判定走 §7.2 事件分支。

## 8. 配置项

`configs/default.json` 新增(`cur_config.json` 同步,代码零魔数):

| 键 | 默认 | 说明 |
|---|---|---|
| `iccard.dev_path` | `/dev/dg_iccard0` | 设备节点路径,驱动最终定名后只改配置不改码 |

防重窗复用既有 `door_open_ms`,不新增键。

## 9. sim 后端与测试要求(完成的定义,缺一不算)

- `drv/iccard/iccard_hal_sim.c`:同 `iccard_hal.h` 接口的模拟后端,提供注入接口
  (如 `iccard_sim_inject(uid, len)`),宿主测试与 PC 演示依赖它——**没有 sim 后端,
  service→FSM→UI 全链路无法在驱动就绪前跑通**。
- 宿主测试(`door-guard/tests/`,WSL gcc 可跑):
  - `test_iccard_proto`:帧校验(magic 错/uid_len 越界丢帧)、HEX 转换、掩码规则
  - `test_card_dedup`:防重窗内忽略/窗外放行/不同卡号不互斥
  - `test_enroll_ic`:重复 → `DG_ERR_DUP_IC`;编辑自身排除自身;成功落库掩码回显
  - `test_fsm_ic`:ST_NORMAL 命中/黑名单/未开 IC/陌生卡四路日志与 reason;v_ic 1:1;
    弹窗阶段忽略不落日志
- 板上验收(驱动就绪后):真卡开门、防重窗(按住不放只开一次)、录入重复卡被拒、
  降级(拔模块 → UI 未就绪提示 → 回插恢复)。

## 10. 驱动侧实现要求与自由度

- **应用层不轮询**:`poll + 阻塞 read` 事件驱动,零 CPU 空转。
- **中断优先**:芯片 IRQ → wait_queue → 驱动内完成寻卡读 UID → 唤醒 poller。
  **允许的退化**:若模块未引出 IRQ 线,驱动内 kthread/timer 周期寻卡(1~5Hz)亦可,
  应用层协议完全不变——两条路线对本文契约无差别。
- **不用 DMA**:单帧 ≤24B,SPI FIFO 一次装下,DMA 配置开销大于传输本身;
  仅当出现大块传输需求再议。
- **驱动不做业务去重**:同卡在场连续上报原样吐帧(seq 递增),去重是应用层业务语义
  (§7.1)。保持驱动"哑",录入/验证/开门各自的去重规则才不会被驱动策略绑死。
- 环形缓冲 ≥8 帧,满丢最旧并 `rx_overruns++`(应用在一个 poll 周期内必然消费,
  缓冲只为吞掉瞬时抖动)。

## 11. 开放项(驱动作者回报后关闭)

1. 设备节点最终名(§2/§8 只改配置)。
2. `card_type` 取值按实际支持的卡型确认(仅诊断,不阻塞)。
3. 有无 IRQ 线(决定 §10 两条实现路线,不阻塞应用层开发)。
4. 同卡持续在场的上报周期(内部轮询路线时),供板上验收对照防重窗。
