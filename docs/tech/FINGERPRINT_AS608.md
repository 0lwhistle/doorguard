# AS608 指纹模组 接入方案设计(UART 读头)

> 状态:**应用层已落地(2026-10-01)**——provider/FSM/录入/UI 全链按本文实现,
> `services/verify/fingerprint/README.md` 是落点索引;剩 DTS 时钟修复 + §6 真机验收。
> 定位:~~方案设计 v1,待确认~~ 已按 IC 卡先例冻结为协议契约
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

**内核侧零开发(2026-09-27 定)**:AS608 是纯 UART 从设备,**不写任何内核驱动**——
应用层直接用现成 `drv/uart/uart_hal`(termios 收发+接收线程,当初即按"指纹/读卡走
串口"预留)打开 `/dev/ttySX`。全部接入成本 = 设备树确认 + 接线 + 配置:

- DTS 里选一个 `status="disabled"` 的**空闲 UART 启用**并确认 pinctrl 引脚复用无冲突;
  **避开 UART2**(调试控制台 1500000,不可动)。
- `WAK` 走内核现成 GPIO 子系统:sysfs export + poll EPOLLPRI(gpio_hal 同款 sysfs
  路线,rootfs 无 libgpiod 不影响内核侧)。
- `VTI` 接 3.3V 供电,纯硬件,不接则 WAK 永远不触发。

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
3. **模块库容量是硬上限**(典型 1000 枚,待手册核对)< 用户上限 2000:**已拍板
   (2026-09-27,决策 A 附议)**——个人项目不担心容量,满了显式提示"指纹库已满"
   即可,不做复杂处置。

## 3. 关键架构决策(**已拍板,2026-09-27**)

### 决策 A:模板存储位置 —— **模组内为主 + DB 加密副本**【已确认】

1:N 验证走模组 Search(纯主控路线 2000 人不可行);DB 副本用于备份/换模组回灌。
容量上限不做复杂处置,满了提示即可(§2.3)。

### 决策 B:**每用户最多 3 枚指纹**【已确认,2026-09-27 用户提出】——DB 模型变更

原方案 users 表单列 `finger_page_id` 不再成立,改为**独立指纹表**:

```sql
CREATE TABLE fingerprints (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     TEXT    NOT NULL,           -- 逻辑外键(users.user_id)
    page_id     INTEGER NOT NULL UNIQUE,    -- 模组库全局页号(DB 是映射唯一事实源)
    finger_vec  BLOB,                       -- 512B 特征副本,AES-256-CTR(同 finger_vec 原策略)
    created_at  INTEGER NOT NULL
);
-- 业务约束:每 user_id 最多 3 行(应用层预检 + 触发器/写入路径兜底)
```

- **users 表原 `finger_vec` 列废弃**:幂等迁移(建表 → 现有数据搬入 fingerprints →
  users 列置 NULL 保留占位,同 avatar 加列先例);spec-database §1/§3 同步修订在
  实现时一并做(本节为事实源,迁移落地前 spec 以本节为准)。
- **上限检查**:录入前 `COUNT(*) WHERE user_id=?` ≥3 → `DG_ERR_FINGER_LIMIT = -37`
  「该用户指纹已达上限(3 枚)」;模组库满 → `DG_ERR_FINGER_FULL = -36`「指纹库已满」。
- **PageID 分配**:全局最小空闲号,删除单枚/删用户释放。1:N Search 命中 PageID →
  fingerprints 反查 user_id → FSM 业务过滤(role/auth_flags),与原方案一致。
- **编辑页 UI 适配**:指纹区显示「已录 n/3」,支持逐枚删除(DeletChar+删行)、
  未满时显示录入按钮——原"修改/录入"单按钮布局不适用,实现时一并改。

### 决策 C:新增错误码【已确认,并按决策 B 扩为两个】

`DG_ERR_FINGER_FULL = -36`(模组库容量满)、`DG_ERR_FINGER_LIMIT = -37`(单用户超 3 枚);
UI 文案表与 web 错误映射同步。查重拒绝仍用既有 `DG_ERR_DUP_FINGER = -24`。

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

用户已选定(uid)→ 提示"请按指纹" → 等下一次按压 → 采集 → **对该用户全部指纹
(≤3 枚)逐一 LoadChar→Match**(PageID 取自 fingerprints 表;具体"逐一 Match"还是
"多枚下载连续比对"在协议冻结时按模组能力定,业务语义不变)→ 任一枚 ok → 成功开门/
全部不匹配 → 失败(`DG_ERR_MISMATCH`,reason=4)。**5s 无 WAK 按下沿 → 取消回普通模式**
(spec §4.4);期间敲弹窗触摸续期规则与人脸一致。

### 5.3 录入(enroll,mode=FINGER)—— 两次按压 + 同指校验 + 双向查重【决策 B 定稿】

```
前置检查:COUNT(fingerprints WHERE user_id) ≥3 → FINGER_LIMIT;
          模组库满(ValidTempleteNum) → FINGER_FULL —— 都不进采集
按压① → GenImg+Img2Tz(Buf1)(不成像且手指未松:隔 150ms 连拍至 3 次,
          2026-10-04——WAK 边沿常赶在指腹贴稳前,首拍 NO_FINGER 白丢按压)
      → EV_ENROLL_PROGRESS(请抬起手指)→ 确认释放后才进查重
        (2026-10-04 用户口径:先提示放开,再提示第二按;等待期间也应用模式命令)
      → Search(全库) 查重
      命中 PageID 反查 user_id:≠本用户 或 ==本用户(与自己已有枚重复) 
          → EV_ENROLL_RESULT(DG_ERR_DUP_FINGER)「指纹重复,录入失败」
      通过 → EV_ENROLL_PROGRESS(提示"请再次按压同一手指")
按压② → GenImg+Img2Tz(Buf2) → EV_ENROLL_PROGRESS(处理中,UI 停 5s 按压计时
          ——处理尾巴可达数秒,计时不停会先弹"已退出"再弹真终态,2026-10-04)
      → **Match(Buf1 vs Buf2) 同指校验**
      不一致 → 提示「两次按压指纹不一致,请用同一手指」,进度回重采
               (主动校验给明确文案;RegModel 对差异过大的特征也会报合成失败,作兜底)
      一致 → RegModel(合成) → Store(分配 PageID)
      → UpChar 读出特征 512B → AES-256-CTR 加密 → INSERT fingerprints(含 finger_vec)
        (UpChar 取不到数据包 → 落**无副本行**继续,录入不回滚;2026-10-03 板上定案)
      → EV_ENROLL_RESULT(OK) → UI 成功提示(已录 n/3)
页面退出/取消 → 撤销录入态;已 Store 的模板 DeletChar 回滚 + 删行,不留孤儿模板
```

查重时点固定在**按压①后**:尽早失败省一次按压;此后到 Store 之间 enroll 单飞无并发,
无需二次查重(用户要求的"录入后检查"由此覆盖——与自己的旧枚、与他人的枚同一判定,
文案统一「指纹重复,录入失败」)。

删除:编辑页**逐枚删除**(DeletChar+删行);删除用户 → 删其全部模板与行(模组通信
失败仍删 DB 行,孤儿由对账暴露,不阻塞删用户)。清空(恢复出厂)→ Empty + 全表清。

### 5.4 降级(同 IC 卡 §7.3)

uart open/握手(读系统参数指令)连续失败 → `EV_SYS_SERVICE_STATE`(指纹模组未就绪),
reason=9 同款;退避重试,恢复上报 READY。模组上电到可响应有稳定期,open 后先握手
再置 READY。

## 6. 边界清单(实现时逐条自测)

- WAK 抖动:驱动侧消抖 30ms;PRESSED..RELEASED 之间单次判定
- 手指按住不放:只判一次,RELEASED 后才允许新流程(防按住连环开门)
- 干手指/按压不实(采集失败确认码):普通模式静默复位等待;录入模式提示重按,不占进度
- **两次按压不同手指**:Match 同指校验(§5.3)明确提示重采;RegModel 合成失败兜底
- **与自己已有枚重复**(同一用户把同一根手指录第二遍):Search 命中反查 ==自己
  → 同样 DUP_FINGER 拒绝,文案与他人重复一致
- 录入中途拔手/超时(单次采集 5s):取消本次,进度回 0
- 模组无响应/串口错:连续 3 次 → 降级;回复后自动恢复
- 查重 Search 与 Store 之间的竞态(录入时他人同时录):enroll 单飞(录入态互斥),无并发
- 容量:录入前查 ValidTempleteNum(待手册核对指令名),满 → FINGER_FULL,不进采集;
  单用户第 4 枚 → FINGER_LIMIT,两者文案区分
- 掉电:Store 成功但 UpChar/落库前掉电 → 模组有模板、DB 无映射 = 孤儿,boot 对账暴露
- 上限 2000 vs 模组容量:满各自显式提示(FULL/LIMIT/USER_LIMIT 三者语义不同,不混用)

## 7. 测试与 mock(完成的定义)

- `uart_hal` 自带 mock 回环:框架级已有,直接用
- `fp_as608` 协议层纯函数单测 `test_fp_proto`:组包/校验和/分帧粘包/应答码解析
  (码值表冻结后写死断言)
- provider 注入层 mock(替代真模组):`test_fp_enroll`(两次按压进度/同指校验不一致
  重采/查重含与己重复/取消回滚/FULL/LIMIT/第 4 枚拒绝/逐枚删除)、
  `test_fsm_finger`(命中/黑名单/未开/陌生四路 reason、v_finger 对 3 枚逐一、按住只判一次)
- DB 迁移测试:users.finger_vec → fingerprints 表幂等迁移(有数据/空库两态)
- 板上验收:真指 1:N 开门、1:1、录入重复指被拒、按住不开两门、拔模组降级提示、
  Search 实测耗时(1000 枚库,<1s 期望,超了要记录再议)

## 8. 配置项(default.json 新增)

| 键 | 默认 | 说明 |
|---|---|---|
| `fp.uart_dev` | `/dev/ttySAC0`(占位) | 模组串口节点,硬件接线后确认 |
| `fp.baud` | `57600` | 待手册核对 |
| `fp.wak_gpio` | `0`(占位) | WAK 引脚号,同 `access.relay_gpio_line` 先例,待硬件确认 |

## 9. 决策记录与剩余开放项

**已拍板(2026-09-27)**:决策 A(模组内+DB 副本,容量满提示即可)、决策 B
(**每用户最多 3 枚** → fingerprints 独立表 + users.finger_vec 幂等迁移废弃 +
编辑页 n/3 UI)、决策 C(DG_ERR_FINGER_FULL=-36 / DG_ERR_FINGER_LIMIT=-37)。
录入两注意点落法:①不同手指 → 按压②后 Match(Buf1,Buf2) 同指校验,明确文案重采;
②查重含与自己重复 → Search 命中反查 user_id,==自己/≠自己 同判 DUP_FINGER
「指纹重复,录入失败」,时点在按压①后。

**剩余开放项(不阻塞 DB/业务层先行开发)**:

1. AS608 随机手册(指令码值/应答码表/容量/波特率/WAK 极性)到位后,协议文档
   `FINGERPRINT_PROTOCOL.md` 按本文框架逐条冻结,复刻 ICCARD_PROTOCOL 的精度;
   1:1 的"≤3 枚逐一 Match"实现取法(逐枚 LoadChar+Match 或多枚下载连续比对)在
   冻结时按模组能力定。
2. WAK 引脚号与串口节点(§8 占位,硬件接线后确认)。
