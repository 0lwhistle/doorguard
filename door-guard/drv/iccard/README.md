# drv/iccard — IC 读卡器设备节点薄封装

> 帧结构与 ioctl 契约:`docs/tech/ICCARD_PROTOCOL.md` §2~§6(驱动作者必读同一份);
> 本层只做节点 open/poll/read/flush,**无线程无业务**——线程/防重窗/降级在
> `services/verify/ic/card_provider`。ICCARD_PROTOCOL §1 分层图即本目录定位。

## 文件

| 文件 | 职责 |
|---|---|
| `iccard_hal.h` | 帧结构(`dg_iccard_frame_t`,24B `_Static_assert` 锁)、ioctl 码、卡号字符串化纯函数 |
| `iccard_hal.c` | 节点操作 + 纯函数实现 |
| `iccard_hal_sim.c` | sim 后端(**仅 `DG_SIM`/`DG_BUILD_TESTS` 编译**,板上不进固件):pipe 一对,读端即句柄,`iccard_sim_inject` 注入帧 |

## 接口速览

```c
int  iccard_hal_open(const char *dev_path);  /* "sim" = 模拟后端 */
int  iccard_hal_poll(int fd, int timeout_ms);/* 1=有帧 0=超时 <0=错 */
int  iccard_hal_read(int fd, dg_iccard_frame_t *out);   /* 一帧 24B */
int  iccard_hal_flush(int fd);               /* 驱动 ioctl;pipe 回退排水 */
void iccard_hal_close(int fd);

bool iccard_frame_valid(const dg_iccard_frame_t *f);    /* magic/uid_len */
int  iccard_uid_to_hex(const uint8_t *uid, uint8_t len, char out[DG_IC_LEN]);
bool iccard_no_valid(const char *card_no);   /* 8~30 偶长大写 HEX(§5) */
void iccard_mask(const char *card_no, char *out, size_t cap); /* ********+末4 */
```

## 已知约束

- **16B UID 超出卡号串承载力**:`DG_IC_LEN=32` 含 `'\0'`,16B UID 的 32 字符
  HEX 放不下——`iccard_uid_to_hex` 对 uid_len>15 返回 `DG_ERR_PARAM`,调用方按
  坏帧丢弃并 WARN。扩上限须先动 `proto/types.h`(DB/事件/用户记录三处口径)。
- 驱动未上线时 `open` 失败是常态(降级重试在 provider);`-EBUSY`(已有读者)
  单独区分——应用内只有一个消费者,出现即查因。

## 测试

`tests/test_iccard_proto.c`:帧校验三态、HEX(4B/15B/16B 拒收)、掩码、
sim pipe 全链路(open→inject→poll→read→flush)。
