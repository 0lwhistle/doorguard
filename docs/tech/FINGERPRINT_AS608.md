# AS608 指纹模组 接入方案设计(UART 读头)

> 定位:**方案设计 v1,待确认**——确认后按 IC 卡先例冻结为协议契约
> (指令码值/应答码表逐条对照随机手册后落 `FINGERPRINT_PROTOCOL.md`)。
> 遵守 `drv/uart/uart_hal.h` 开头既定纪律:**帧协议等硬件手册,不臆造**;
> 本文所有 AS608 指令/参数均标注"待手册核对",框定的是架构与业务语义。
> 业务细则权威:`spec-auth-business.md`、`spec-database.md`。

## 1. 硬件接口与接线

| 模块引脚 | 方向 | 接法 | 说明 |
|---|---|---|---|
| `3V3` | — | 3.3V | 模组主电源 |
| `GND` | — | GND | |
| `TXD` | 出 | RK3576 UART RX | 模组应答/数据,默认 **57600 8N1**(待手册核对,可指令改) |
| `RXD` | 入 | RK3576 UART TX | 主控指令 |
| `WAK` | 出 | GPIO 输入(建议上拉) | **触摸感应输出**:手指放上有效(极性待实测)。这是"指纹按下"的物理事件源,驱动采集流程靠它,不轮询模组 |
| `VTI` | — | 3.3V | 触摸感应电路供电(VTouch in),不接则 WAK 不工作 |

要点:`VTI` 不接是最常见"WAK 永远不触发"的装配坑;`WAK` 是选配引脚里唯一必须接的
——没有它就只能周期发指令探测手指(浪费串口+响应慢),本方案按 WAK 事件驱动设计。

## 2. 预想评审(对"业务逻辑与 IC 卡类似"的结论)

**成立**:线程模型、事件路由、防重窗、录入查重、验证按钮 1:1——与 IC 卡方案完全同构
(见 ICCARD_PROTOCOL.md §7),FSM 分支表同一张。且指纹比 IC 卡**更天然离散**
(WAK 按下沿=明确的一次"在场",没有"卡一直贴着"的连续上报问题),IC 的防重窗在指纹
场景退化为"一次按压(PRESSED→RELEASED)只判一次",更简单。

**三处指纹特有的差异**,不是对预想的推翻,是必须补的决策:

1. **录入是两次按压**:AS608 标准录入 = 两次采集 → 模块内合成模板 → 存储。
   UI 要有进度语义(EV_ENROLL_PROGRESS 两步),且**查重插在第一次按压后**(§5),
   不能录完两次才发现重复。
2. **模板存模块内**,不是全存主控(§3,推荐决策 A):验证 1:N 在模组内完成,
   主控 DB 存加密副本做备份/迁移。这与 spec-database 的 `finger_vec` 列并存不冲突。
3. **模块库容量是硬上限**(典型 1000 枚,待手册核对)< 用户上限 2000:第 1001 枚
   指纹录入必须显式失败(新错误码,§6),提示"指纹库已满",而不是静默失败。

## 3. 关键架构决策(请拍板)

### 决策 A:模板存储位置 —— 推荐 **模块内为主 + DB 加密副本**

| | A:模块内 PageID + DB 副本(推荐) | B:纯主控(特征全存 SQLite) |
|---|---|---|
| 1:N 验证 | 模组 Search,模组内比对,单次 <1s(待实测) | 需主控实现指纹比对算法或逐枚下载 Match,2000 人不可行 |
| 查重 | 录入时 Search 命中检测,天然 O(库) | 同左,须算法在主控 |
| 换模组/恢复 | DB 副本 DownChar 回灌,可迁移 | 天然可迁移 |
| 与 2000 上限 | 受模组容量约束(§2.3) | 无约束但验证路径不可行 |

A 的代价(容量上限 + PageID 映射)是可管理的,B 的 1:N 验证是**不可行**的,故选 A。
`finger_vec` 列语义微调:由"验证数据源"变为"副本(备份/换模组回灌)",加密策略不变。

### 决策 B:PageID 分配 —— 推荐 **递增分配 + users 表映射列**

`users` 表幂等迁移加列 `finger_page_id INTEGER`(NULL=未录指纹;先例:avatar 加列)。
分配取当前最小空闲号,删除用户时 `DeletChar` 释放并置 NULL。DB 是映射唯一事实源,
模组库只是缓存——boot 时可对账(ValidTempleteNum 与 DB 计数不符 → 日志告警,
不自动重建,人工介入)。

### 决策 C:新增错误码 —— `DG_ERR_FINGER_FULL = -36`(指纹模组库容量已满)

现 err.h 有 DUP_FINGER(-24)查重、MISMATCH(-35)1:1 不匹配,独缺容量满。
同时 UI 文案表与 web 错误映射同步加一条。

## 4. 分层落点(五层栈)

| 层 | 新增 | 职责 |
|---|---|---|
| drv/uart | **复用,零改动** | `uart_hal` 收发+接收线程+mock 回环,正是为此预留(注释原话"指纹/读卡等手册") |
| drv/gpio | 扩展**输入**能力 | 现仅门控输出语义;加 `gpio_hal_wait_edge(line, timeout_ms)`(sysfs value + poll EPOLLPRI,同 sysfs 路线;实测不稳则退化 10Hz 电平轮询,接口不变)。WAK 引脚号进配置(出厂占位 0,同 relay_gpio_line 先例) |
| services/verify/fingerprint | `fp_as608.c` 协议层 | AS608 帧组包/解析/校验和、指令序列状态机(纯函数,宿主可单测);**不持线程** |
| services/verify/fingerprint | `fp_provider.c` | **持业务线程**:WAK 边沿等待 → 采集/比对序列(经 uart_hal)→ 发事件;防重、降级、PageID 映射缓存 |
| 事件契约 | 新增 2 个(HAL 域) | `EV_FINGER_MATCH_1N`(user_id, score)、`EV_FINGER_VERIFY_11`(user_id, ok, score)——与 `EV_VISION_MATCH_1N/VERIFY_11` 同构;`EV_FINGER_STATUS`(PRESSED/RELEASED/ERROR)契约已有,直接用 |
| access/auth_fsm | 指纹分支 | 同 ICCARD_PROTOCOL §7.2 表:ST_NORMAL 1:N 开门 / ST_ADMIN_AUTH 管理员检索 / v_finger 子步 1:1 / 其余状态忽略,method=2 |

线程/路由语义与 IC 卡同款修正:**provider 线程永不停**,进验证弹窗只是 FSM 不走普通
分支;`v_finger` 子步("请按指纹")恰恰依赖同一条事件流。

## 5. 业务流程语义(事件流)

### 5.1 普通模式 1:N(ST_NORMAL)

```
WAK 按下沿(消抖 30ms)→ EV_FINGER_STATUS(PRESSED)
  → GenImg → Img2Tz(Buf1)              [失败(无手指/质量差)→ ERROR 事件,静默复位等待]
  → Search(Buf1, 全库) → 命中(PageID, score) → 反查 DB(user_id)
      → FSM:role=2 黑名单→失败(reason=2)/未开指纹→失败(6)/命中→开门(0),日志 method=2
      → 一次按压只判一次:RELEASED 前不再触发新流程(同 IC 防重窗精神)
  → 未命中 → 失败+日志(user_id=NULL,reason=1 陌生人)
```

### 5.2 验证按钮 1:1(v_finger 子步)

用户已选定(uid)→ 提示"请按指纹" → 等下一次按压 → 采集 → LoadChar(该用户
PageID→Buf1)与本次特征 Match(或等价单页 Search,协议冻结时定)→ ok → 成功开门/
不匹配 → 失败(`DG_ERR_MISMATCH`,reason=4)。**5s 无 WAK 按下沿 → 取消回普通模式**
(spec §4.4);期间敲弹窗触摸续期规则与人脸一致。

### 5.3 录入(enroll,mode=FINGER)

```
EV_ENROLL_REQUEST(FINGER) → 置录指纹态
  按压① → GenImg+Img2Tz(Buf1) → Search(全库) 查重
      命中 PageID 映射 ≠ 本用户 → EV_ENROLL_RESULT(DG_ERR_DUP_FINGER)「指纹重复」
      模组库满 → EV_ENROLL_RESULT(DG_ERR_FINGER_FULL)「指纹库已满」
      通过 → EV_ENROLL_PROGRESS(1/2,提示"再次按压")
  按压② → GenImg+Img2Tz(Buf2) → RegModel(合成) → Store(分配 PageID)
      → UpChar 读出特征 512B → AES-256-CTR 加密落 DB(finger_vec + finger_page_id)
      → EV_ENROLL_RESULT(OK) → UI 成功提示
页面退出/取消 → 撤销录入态(已 Store 的模板 DeletChar 回滚,不留孤儿模板)
```

删除用户 → `DeletChar(PageID)` + 清列(模组通信失败时仍删 DB 行,模组留孤儿模板
由对账机制暴露,不阻塞删用户)。清空接口(恢复出厂)→ `Empty` + 全列清 NULL。

### 5.4 降级(同 IC 卡 §7.3)

uart open/握手(读系统参数指令)连续失败 → `EV_SYS_SERVICE_STATE`(指纹模组未就绪),
reason=9 同款;退避重试,恢复上报 READY。模组上电到可响应有稳定期,open 后先握手
再置 READY。

## 6. 边界清单(实现时逐条自测)

- WAK 抖动:驱动侧消抖 30ms;PRESSED..RELEASED 之间单次判定
- 手指按住不放:只判一次,RELEASED 后才允许新流程(防按住连环开门)
- 干手指/按压不实(采集失败确认码):普通模式静默复位等待;录入模式提示重按,不占进度
- 录入中途拔手/超时(单次采集 5s):取消本次,进度回 0
- 模组无响应/串口错:连续 3 次 → 降级;回复后自动恢复
- 查重 Search 与 Store 之间的竞态(录入时他人同时录):enroll 单飞(录入态互斥),无并发
- 容量:录入前查 ValidTempleteNum(待手册核对指令名),满 → FINGER_FULL,不进采集
- 掉电:Store 成功但 UpChar/落库前掉电 → 模组有模板、DB 无映射 = 孤儿,boot 对账暴露
- 2000 用户 vs 模组容量:提示语义明确到"指纹库已满"而非"添加失败",区分于 USER_LIMIT

## 7. 测试与 mock(完成的定义)

- `uart_hal` 自带 mock 回环:框架级已有,直接用
- `fp_as608` 协议层纯函数单测 `test_fp_proto`:组包/校验和/分帧粘包/应答码解析
  (码值表冻结后写死断言)
- provider 注入层 mock(替代真模组):`test_fp_enroll`(两次按压进度/查重/回滚/容量满)、
  `test_fsm_finger`(命中/黑名单/未开/陌生四路 reason、v_finger、按住只判一次)
- 板上验收:真指 1:N 开门、1:1、录入重复指被拒、按住不开两门、拔模组降级提示、
  Search 实测耗时(1000 枚库,<1s 期望,超了要记录再议)

## 8. 配置项(default.json 新增)

| 键 | 默认 | 说明 |
|---|---|---|
| `fp.uart_dev` | `/dev/ttySAC0`(占位) | 模组串口节点,硬件接线后确认 |
| `fp.baud` | `57600` | 待手册核对 |
| `fp.wak_gpio` | `0`(占位) | WAK 引脚号,同 `access.relay_gpio_line` 先例,待硬件确认 |

## 9. 待你确认的决策点(回复后冻结协议文档)

1. **决策 A**(模板:模块内+DB 副本)是否通过 —— 决定 `finger_vec` 列语义微调与
   spec-database 的一句话修订;
2. **决策 B**(users 表加 `finger_page_id` 列,幂等迁移)是否通过;
3. **决策 C**(新错误码 `DG_ERR_FINGER_FULL=-36` + UI/web 文案)是否通过;
4. AS608 随机手册(指令码值/应答码表/容量/波特率/WAK 极性)提供后,协议文档
   `FINGERPRINT_PROTOCOL.md` 按本文框架逐条冻结,复刻 ICCARD_PROTOCOL 的精度。
