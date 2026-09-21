# 架构 v2 方案

> 状态:**已评审通过(2026-09-20,决议见 §10)**。本文并入 `PROJECT_PLAN.md` §三
> (分层图与目录索引以本文为准);M1 机械迁移随决议执行。
> 对"重构方向初稿"的完整评审结论见 §0。

---

## 0. 相对初稿:保留了什么,改了什么

**保留(方向不变):**
五层栈方向(UI → services → modules → drv → components);
drv 做总线级薄封装;holder 管 modules / registry 管 services 的双注册表 +
必须/可选语义;LVGL 两条纪律;配置双文件(deconfig→default + cur_config,按项恢复);
数据库按页预加载;人脸"质量评分闸门后再识别"。

**修正(评审结论落地):**

| # | 初稿 | 修订 | 原因 |
|---|---|---|---|
| 1 | 耗时低实时任务进 tasker | 耗时任务一律独立线程(按需或常驻),tasker 只收 ≤500ms 短任务 | 队头阻塞;与既定 500ms 纪律冲突 |
| 2 | 组件名 `register` | `registry` | C 保留关键字 |
| 3 | `vertify` / `meun` / `editLine` | `verify` / `menu` / `editline` | 标识符永久传播,先改名的成本最低 |
| 4 | `stream(摄像头推流)` | `capture(取流)`;"推流"一词留给对外网络推流 | 消除本地取流/RTSP 推流的歧义 |
| 5 | 无契约层 | 补 `proto/`(事件/类型/校验,唯一横切层) | "模块间只经 proto 通信"要有落点 |
| 6 | 无 tests | `tests/` 落位(现有 20 项全部保留并扩展) | 完成的定义 = 代码+测试+README+示例 |
| 7 | 无活体 | `services/liveness` 显式化,作为人脸管线的强制阶段 | 产品定义 = 1:N + 动作活体;防照片攻破 |
| 8 | cur_config 放源码树 | 移到设备数据分区 `/userdata/doorguard/` | A/B 升级/重刷不丢用户配置 |
| 9 | 配置"懒加载分页" | 配置全量进内存 + 防抖落盘;分页只属于数据库 | 配置是小 JSON,分页是把简单事搞复杂 |
| 10 | 线程"原子标志位睡眠" | condvar/eventfd + 原子状态,先置位后 signal,带超时兜底 | 仅标志位有丢唤醒竞态 |
| 11 | 无监督机制 | main 转看门狗 + 降级矩阵 | "防失控"要有机制不能只有原则 |
| 12 | 线程清单无主人 | 线程↔服务归属表(§3.3),一条线程唯一归属 | 人脸/配置线程在服务清单里查无此人 |
| 13 | OTA 常驻线程 | 按需线程(下载期间存在,完成即退出) | 省一线程 |
| 14 | 无 NTP | `services/ntp` 显式立项 | access_logs 可信度依赖时间;上版刚修过"NTP 从未装配" |
| 15 | services 只经 modules | services 允许直用 drv(npu 白名单)与只读跨服务查询(§1) | vertify/face 要用 NPU,原规则打不通 |

---

## 1. 分层总览与依赖规则

```
┌──────────────────────────────────────────────────────────┐
│ ui/        页面·控件·桥接(只发/收事件,不做业务决策)          │
├──────────────────────────────────────────────────────────┤
│ services/  顶层业务(注册进 registry)                       │
│   capture  vision  liveness  verify  access  enroll       │
│   database  config  web  ota  mdns  ntp  ui                │
├──────────────────────────────────────────────────────────┤
│ modules/   设备/中间层(注册进 holder)                      │
│   camera  display  touch  as608  mfrc522  sqlite  net      │
├──────────────────────────────────────────────────────────┤
│ drv/       总线级薄封装(可替换;PC 模拟器同接口 sim 后端)     │
│   i2c  spi  uart  gpio  pwm  npu                           │
├──────────────────────────────────────────────────────────┤
│ components/ 核心机制:tasker event_bus holder registry logger│
├──────────────────────────────────────────────────────────┤
│ proto/      跨层契约:events types valid err(唯一横切层)     │
└──────────────────────────────────────────────────────────┘
  平台:官方 SDK(kernel 6.1 + rkaiq + rknpu2 + rga + mpp + libmali + buildroot)
  third_party:mongoose / cjson /(sqlite3 走系统库,NEEDED 契约见 modules/sqlite README)
```

**依赖白名单(违者评审不收):**

| 层 | 可依赖 |
|---|---|
| ui | proto(仅事件与类型);经 `ui/bridge` 订阅/发布,禁 include services/modules 头 |
| services | components、modules、proto、third_party;**drv 仅白名单:npu → vision** |
| modules | components、drv、proto。camera/display/net 直接封装内核接口(V4L2/DRM/netdev),不经 drv |
| drv | components/logger、proto/err、内核头 |
| components | proto(err/types)仅 |
| proto | 无(仅 libc) |

**跨服务调用(2026-09-20 已评审通过):**

- 写与命令:一律 event_bus 异步,请求/响应事件带 correlation id。
- 只读查询:**高实时、高性能路径允许直调**目标服务暴露的**无阻塞 const 接口**。
  判定标准:该调用处于逐帧/逐次验证等热路径、异步事件往返不可接受,且被调方
  纯读内存。初始登记两项——database 的特征快照、config 的配置读取;新增直调
  必须先在本文档登记(路径 + 理由)再落码,评审不收未登记的直调。
  已登记:
  - `ui/widgets/dg_avatar`、`enroll_service` → `modules/sqlite` 头像读写
    (KB 级 BLOB 独立于认证热路径,经专用接口,不入 user_rec_t;2026-09-21);
  - 视觉/编辑页 → `modules/jpeg` 编解码(拍摄时一次编码 / 显示时解码,几 ms);
  - `page_home` → `net_info_primary_ipv4`(主页网络图标 1s 轮询,纯 getifaddrs
    无阻塞;外网探测的阻塞版 is_online 不得在 UI 线程调;2026-09-21)。

---

## 2. 目标目录树

```
door-guard/
├── app/                      # 装配启动:holder+registry 装配、依赖检查、启动顺序;
│   └── main.c                #   初始化完成后 main 线程转看门狗(§3.4)
├── proto/                    # 跨层契约:events.{h,c} types.{h,c} valid.{h,c} err.h
│                             #   (tasker/event_bus/holder 实现迁出 → components/)
├── components/               # 核心机制,无业务,pthread port
│   ├── tasker/               #   ≤500ms 短任务 + 周期/次数任务;3 分级 lane + 优先级
│   ├── event_bus/            #   异步分发(独立分发线程),EV_<域>_<动作>
│   ├── holder/               #   modules 注册表:状态机 + 依赖检查(HOLDER_ERR_DEPENDENCY)
│   ├── registry/             #   services 注册表(与 holder 同状态机;原"register")
│   └── logger/               #   日志(原 proto/dg_log.* 迁入,API 前缀 dg_log_ 不变)
├── drv/                      # 总线级薄封装,边界错误内部处理,PC 模拟器同接口
│   ├── i2c/  spi/  uart/  gpio/  pwm/  npu/
│                             #   i2c/spi/pwm 新增;uart/gpio 自 hal/ 迁入;npu 自 hal/ 迁入
├── modules/                  # 设备/中间层模块;注册、注销、回调注册、操作集、状态查询
│   ├── camera/               #   IMX415 V4L2/ISP 设备封装(内核接口,不经 drv)
│   ├── display/              #   MIPI DRM 显示封装(同上)
│   ├── touch/                #   GT9xx 触摸(I2C)——新增模块,喂 services/ui 的 indev
│   ├── as608/                #   指纹(UART)——硬件接入时按此落位
│   ├── mfrc522/              #   IC 读卡(SPI)——同上
│   ├── sqlite/               #   libsqlite3 薄封装(连接/迁移/stmt 助手),原 hal/storage
│   ├── jpeg/                 #   头像 JPEG 编解码(libjpeg 内存↔内存薄封装,2026-09-21)
│   └── net/                  #   netif/网口信息/socket 助手(mdns/ntp/ota/web 迁出 → services/)
├── services/                 # 顶层业务;每个服务在归属表中认领线程(§3.3)
│   ├── capture/              #   取流线程:rkaiq 3A + V4L2 → NV12 dma_buf 环形缓冲,分发 fd
│   ├── vision/               #   人脸管线线程:检测→质量闸门→(活体)→特征→1:N/1:1;后端可插拔契约不变
│   ├── liveness/             #   动作活体状态机(随机动作序列);纯逻辑与图像判定分离,无独立线程
│   ├── verify/               #   认证编排 + provider(接口名保留 auth_provider.h)
│   │   ├── face/  fingerprint/  ic/  password/
│   ├── access/               #   门控决策 + 继电器 + 验证日志唯一出口
│   ├── enroll/               #   用户/特征录入(查重:特征/卡号跨用户唯一,错误码区分)
│   ├── database/             #   业务数据:users / access_logs;单写者线程(§4.2)
│   ├── config/               #   配置服务:default.json 模板 + /userdata 现用配置(§4.1)
│   ├── web/                  #   上位机 HTTP+WS(mongoose;web_auth/web_session;前端工程迁入 frontend/)
│   ├── ota/                  #   A/B 升级:按需线程,流式下载→备用槽→置标志→重启
│   ├── mdns/                 #   通告/探测防重名/IP 变化重通告;tasker 周期任务,无独立线程
│   ├── ntp/                  #   时间同步;tasker 周期任务(小时级),无独立线程
│   └── ui/                   #   LVGL 宿主:lvgl 线程 + DRM 刷新 + 触摸 indev + tick
├── ui/                       # 界面(现有组织保持:bridge/presenters/pages/widgets/navigator/lang/font,
│                             #   单复数不做无谓更名);验证流程 = 主页弹窗流,不设独立整页(spec-ui)
├── configs/                  # 仓库内只放 default.json(默认配置模板,入库);cur_config 不入库(§4.1)
├── models/                   # 模型清单/校验和(sha256);SDK 二进制不入 git,板上部署 /usr/lib(整目录)
├── tests/                    # PC 可跑纯逻辑测试(现有 20 项保留;新模块随做随补)
├── sim/                      # PC 模拟器宿主(DG_SIM;#ifdef 只许出现在 HAL sim 后端与 main 装配)
├── tools/  scripts/          # 不变
├── third_party/              # mongoose、cjson;sqlite3 走板上系统库(sysroot 同源)
└── build/                    # 产物:board/ 与 pc/,gitignore,不入库
```

---

## 3. 核心机制

### 3.1 双注册表

- `holder`(modules)/ `registry`(services)同一套状态机:REGISTERED → INITIALIZING →
  READY / ERROR / DISABLED;依赖检查防循环,必须项失败拒绝启动,可选项失败置 DISABLED 降级。
- registry 额外记录**线程归属**(§3.3),供看门狗巡检;服务注销仅限停用时置位,
  运行期不做真注销(设备形态无热插拔,锁开销不值得)。

### 3.2 任务分类学(修订版)

| 任务形态 | 执行位置 | 结果返回 |
|---|---|---|
| 短任务 ≤500ms,低实时 | tasker 分级 lane | event_bus 回调 |
| 短任务,高实时 | 调用者上下文同步执行(lvgl 线程内上限几 ms,超了就走上一行) | 直接返回 |
| **耗时/长阻塞(不再按实时性分)** | **独立线程:一次性任务按需创建用完即毁(OTA);周期性常驻(见归属表)** | event_bus 回调 |
| 常驻任务 | 专属常驻线程(归属表) | — |
| 周期任务 | tasker 周期节点(mdns 通告、ntp、心跳、配置防抖落盘) | event_bus 可选 |
| 单次/数次 | tasker 次数节点(TASK_CNT) | event_bus 回调 |

### 3.3 线程规范与归属表

**睡眠/唤醒规范:** 每线程 = 原子状态位(run/idle/stop)+ 阻塞原语(condvar 超时等待或
eventfd/epoll)。先改状态再 signal;对外成对提供 `<svc>_sleep()` / `<svc>_wake()`;
所有等待必须带超时兜底;**禁止仅凭标志位忙等**。每常驻线程周期性刷新心跳时间戳,供看门狗读。

| 线程 | 归属 | 生命周期 | 阻塞点 / 唤醒 |
|---|---|---|---|
| main | app(装配 → 看门狗) | 常驻 | 低频轮询(秒级) |
| event_bus 分发 | components/event_bus | 常驻 | 队列 condvar |
| tasker hi / normal / low | components/tasker | 常驻 | 定时器 + 队列 |
| capture 取流 | services/capture | 常驻 | V4L2 dqbuf 阻塞 |
| vision 人脸管线 | services/vision | 常驻,按页面条件运行(主页普通模式才跑 1:N) | 帧可用信号;弹窗期 1:N 挂起标志位 |
| database 写者 | services/database | 常驻 | 请求队列 condvar |
| ui / lvgl | services/ui | 常驻 | lv_timer_handler 30~50ms 节拍 + indev |
| web | services/web | 常驻(net 不可用不启动) | mg_mgr_poll |
| ota | services/ota | **按需**,完成即退出 | 下载流 |
| mdns / ntp / config | services/mdns / ntp / config | **无独立线程**,tasker 周期/防抖任务 | — |

### 3.4 看门狗与降级矩阵

main 装配完成后进入看门狗循环:周期(5s)检查 registry 心跳与 holder 状态。
可选服务超时 → 记日志、尝试重启一次、失败则 DISABLED 并发 UI 提示事件;
必须服务超时 → 安全停机(继电器复位、屏幕提示、串口日志)。硬件看门狗后续接入。

| 项 | 必须性 | 不可用时 |
|---|---|---|
| display / touch / ui / sqlite+database | 必须 | 拒绝启动(安全停机) |
| camera + vision + liveness(npu 同) | 可选(降级) | 禁人脸验证;密码/指纹/IC 照常;UI 提示 |
| as608 / mfrc522 | 可选(降级) | 对应验证方式置灰 |
| net(及 web/ota/mdns/ntp) | 可选(降级) | 本地功能全可用;远端功能停 |

### 3.5 LVGL 纪律(沿用,不改动)

耗时操作提交 tasker,完成后 `lv_async_call()` 回 UI;外部线程禁直调 LVGL,一律
`lv_async_call()`;UI 只发/收事件,业务决策不进 UI 层。

---

## 4. 数据与配置

### 4.1 配置服务(services/config)

- 仓库 `configs/default.json` = 默认模板(只读);设备 `/userdata/doorguard/cur_config.json`
  = 现用配置,程序运行**只读它**;首次启动不存在则从 default 复制生成。
- 原子写 = 写临时文件 → fsync → rename;启动全量载入内存,`set` 即内存生效 + 500ms 防抖落盘。
- 恢复默认(单项/全部)= 从 default 拷对应键 → 同一原子写路径。带 `version` 字段,未知键忽略(向前兼容)。
- DB 的 device_config 表**冻结**:存量读取保留,新配置项一律进 JSON,避免两处真相(待拍板 §10-2)。

### 4.2 数据库服务(services/database)

- **单写者线程 + 请求队列**(命令模式),杜绝 SQLITE_BUSY 的多线程乱象;WAL +
  busy_timeout,web 查询与门禁日志写互不阻塞。
- users / access_logs 按页查询,前后各预加载两页(2000 上限)。
- 所有写路径(注册、删改、日志)经队列异步,event_bus 回调结果;access_logs 的唯一
  写入方是 services/access。

### 4.3 特征缓存

人脸特征表(≤2000 条,量小)启动后**全量载入内存**,RWLock 保护;database 暴露只读
快照接口,verify 直读(§1 白名单唯二直调之一);增删改走写队列,落库同时增量更新缓存。
**禁止每帧查库。**

---

## 5. 认证管线

```
capture 帧 ─→ vision:检测 → 质量闸门(评分达标才有资格) → liveness 动作判定
            → 特征提取 → 1:N 检索(仅"开启人脸且非黑名单")
            ─EV_AUTH_RESULT→ access:门控决策 + 继电器 + access_logs(唯一出口)
                           ─EV_→ ui/bridge → lv_async_call 绘制(绿/红框 + 结果弹窗)
```

- liveness 是**强制阶段**,随机动作序列状态机;纯逻辑(序列/超时)与图像判定分离,PC 可测。
- 模式规则沿用 spec-auth-business:普通 1:N(1.5s 未命中/黑名单→失败);管理员模式 5s
  无人脸自动回普通;点验证 → ID + 按 users 开启的方式选 1:1;弹窗期间推流照常、1:N 挂起。
- 密码验证 = ID + 密码(ID+密码可重复);新用户必须设密码否则禁止添加——规则唯一权威
  仍是 `proto/valid.c`。

---

## 6. 网络服务族

- `modules/net`:netif/网口信息/socket 助手(内核接口层)。
- `services/web|ota|mdns|ntp`:自现 modules/net/{web,ota,mdns,ntp} 平移,职责不变;
  远程监控画面 = web 服务从 capture 环冲取帧(WS 快照/MJPEG),RTSP 暂缓(§10-3)。
- 依赖链:web/ota/mdns/ntp → modules/net;net 不可用 → 四者不启动(降级矩阵)。

---

## 7. 命名规范

- 目录/文件:小写单数(`component/editline`);模块 API `module_action()` / `module_type_t`。
- 用词统一:**verify**(非 vertify)、**menu**(非 meun)、**registry**(非 register)、
  **capture**(本地取流;"推流"只指对外网络推流)。
- 事件 `EV_<域>_<动作>`;注释解释"为什么";label 一律 `_("原文")`,语言包 `ui/lang/*.json`。

---

## 8. 迁移映射(现 → 目标)

| 现路径 | 目标路径 | 说明 |
|---|---|---|
| proto/tasker、proto/event_bus、proto/holder | components/{tasker,event_bus,holder} | port 不动,挪家 |
| proto/dg_log.{h,c} | components/logger/ | API 前缀 dg_log_ 不变 |
| proto/{events,types,valid,err} | proto/(原地) | 契约层正名 |
| — | components/registry | 新增(services 注册表) |
| hal/{uart,gpio} | drv/{uart,gpio} | 平移 |
| hal/npu | drv/npu | 平移 |
| — | drv/{i2c,spi,pwm} | 新增(随 touch/mfrc522 接入) |
| hal/camera | modules/camera | 升格设备模块 |
| hal/display | modules/display | 升格设备模块 |
| hal/storage | modules/sqlite | 更名对齐 |
| — | modules/touch、as608、mfrc522 | touch 新增;指纹/读卡硬件接入时按位落座 |
| modules/capture | services/capture | 平移 |
| modules/vision | services/vision | 平移(后端可插拔契约不变) |
| modules/liveness | services/liveness | 平移 + 管线内显式阶段 |
| modules/access | services/access | 平移 |
| modules/enroll | services/enroll | 平移 |
| auth/{face,finger,card} + auth_provider.h | services/verify/{face,fingerprint,ic} + password | provider 归位,接口名不变 |
| modules/net/net_info.* | modules/net | 留守 |
| modules/net/{web,ota,mdns,ntp} | services/{web,ota,mdns,ntp} | 拆出;web 前端工程随迁 services/web/frontend |
| config/cfg.{h,c} + configs/device.json | services/config + configs/default.json + /userdata/cur_config.json | 双文件方案落地,含一次性迁移旧 device.json |
| ui/(bridge/presenters/pages/widgets/navigator/lang/font) | 原地保留 | 单复数不做无谓更名 |
| tests/(20 项) | tests/(原地) | 随迁移改 include 路径,全程保持可跑 |

## 9. 迁移策略与阶段

- **M0(本文)**:评审通过 → 并入 PROJECT_PLAN §三 + architecture.md,更新目录索引。
- **M1 纯机械移动,零行为变更**:git mv + CMake + include 路径一次到位;
  验收 = 全部 tests 绿 + frontend_check + PC 模拟器全流程回归(test_verify_flow 等)。
  M1 期间禁止夹带任何功能改动。——**✅ 2026-09-20 完成**。
- **M2 行为升级(每项独立提交、可单独回滚)**——**✅ 2026-09-21 完成**:
  ① config 双文件 + 原子写 + 旧配置迁移(DB 冻结;`546833c`);
  ② DB 特征缓存 + storage_features_ro 只读快照(增量同步/fail-closed,`676bc07`;
     遗留:单写者请求队列未做,WAL 既有);
  ③ registry 装配 + main 转看门狗 + 降级矩阵(重启一次→禁用→EV_SYS_SERVICE_STATE 通知;
     必需服务安全停机,`dc306a6`;遗留:UI 对该事件的提示渲染未接);
  ④ ota 按需写线程流水线 + 进度事件做实(`2a8800e`)。
- **M3 补缺**:touch 模块化、as608/mfrc522 硬件接入时按新架构落位(不再产生新欠账)。
- 时机建议:B7 人脸联调收尾后的空窗期执行 M1(半天量级);M2 各项可择机穿插。

## 10. 评审决议(2026-09-20,已全部拍板)

1. **跨服务只读直调:放宽**(按"高实时/高性能路径"判定,登记制,见 §1)——用户决议:
   高实时性、高性能要求的读路径可放宽;初始登记 database 特征快照、config 读取两项。
2. **device_config 表冻结**:新配置只进 JSON,存量一次性迁移 —— 通过。
3. **RTSP 暂缓**:远程预览先走 WS 快照(复用现有 WS 推送通道,周期推 JPEG)—— 通过。
4. **迁移时机:立即**——本文档即日生效,M0(并入 PROJECT_PLAN/architecture.md)与
   M1(机械迁移)已同日执行完成:目录/include/CMake 全量迁移,WSL 宿主全新构建
   **22/22 测试通过、0 警告**;M2 行为升级逐项独立提交、可单独回滚。

---

## 11. 落地自查清单(新会话开工时按此核验,2026-09-21 基准)

全部通过 = v2 落地完好;任一不符 = 有人动过,先查 git log 再继续开发。

**① 提交链与远程**(`git status -sb` + `git log --oneline -12`):

- `## master...origin/master` **无 ahead/behind**(工作树干净且已推送);
- 提交历史未被人重写(不查具体哈希——哈希每次提交都会变,查形态):
  `git log --oneline -12` 应能看到 `feat(npu)`/`feat(vision)`/`feat(ui)`/`docs(arch)`/
  `refactor(arch)` 等本项目前缀,且最早的 M1/M2 提交仍在(若 `91c4990`、
  `dc306a6` 这类近期哈希一个都找不到 = 历史被重写,先查 git reflog)。

**② 目录形态**(v2 五层栈,`ls door-guard`):

- 应有:`app ui proto components drv modules services configs models tests sim tools third_party`;
  components 下有 `tasker event_bus holder registry logger`;
  services 下有 `capture vision liveness verify access enroll config web ota mdns ntp`;
  modules 下只有 `camera display sqlite net`;
  **drv 下 `npu/` 已填**(`npu_model.c` rknn 薄封装 + `npu_pre.c` RGA letterbox;
  全仓唯一 include `<rknn_api.h>` 的文件在 `npu_model.c`);
  **`services/vision/` 有三个后端**:`vision_rknn.c`(主线)/`vision_rockiva.c`(备选)/
  `vision_sim.c`(PC),外加纯算法单元 `rknn_face.c`(宿主可测);
- **不应有**:`hal/`、`auth/`、顶层 `config/`、
  `modules/{capture,vision,liveness,access,enroll}`、`modules/net/{web,ota,mdns,ntp}`、
  `proto/{tasker,event_bus,holder,dg_log.*}`(均已在 M1 迁走)。

**③ 测试**(WSL 宿主全新构建):`ctest` **28/28 全绿**(22 原有 + test_feat_cache /
test_registry / test_ota + **test_rknn_face(解码/NMS/对齐/余弦)/
test_npu_pre(letterbox 坐标数学)/ test_enroll_flow(录入链路端到端)**);
零警告以 python 全字节扫描构建日志判定
(`grep -c warning` 对未落盘日志有竞态假象,不可作准),
且 `tests/test_i18n.c` 会拦住裸中文与字体缺字形。

**④ 关键行为红线**(`grep` 核验):

- `services/config/cfg.c` 中 **不得出现 `db_config_set`**(DB 冻结;只允许
  `db_config_get` 做首启迁移读取);
- 配置运行时文件只在 `/userdata/doorguard/cur_config.json`(板)/ `sim/data/`(sim),
  **不得回到源码树或 /etc 写入**;
- 全源码树(app ui proto components services modules drv tests)**不得有
  `#include "hal/…"`、`#include "auth/…"` 等 M1 前旧路径**;
- `git ls-files door-guard/models` 只有 `README.md` 与 `sha256sums.txt`
  (模型二进制 .rknn/.data 永不入库,被 .gitignore 挡住);
- `git push` 正常即可,**不需要 --force**(远程旧 lineage 已于 2026-09-21 替换,
  其他旧克隆须 `fetch + reset --hard` 对齐,勿 pull)。

**⑤ 文档同步态**:DEVLOG 顶部为 2026-09-21 当日多条(自组 rknn 路线开工/检测上板/
识别接通/UI 反馈三项/编辑页三缺陷);PROJECT_PLAN 快照与 §3.2 数据流、§3.4 目录、
§4.3 人脸方案已按自组 rknn 主线更新;视觉细节在 `services/vision/README.md`,
模型清单/重转/实测在 `door-guard/models/README.md`;B7 交接文档顶部已标注路线变更。
本文 §9 标注 M1/M2 完成与遗留(DB 单写者队列、EV_SYS_SERVICE_STATE 的 UI 渲染、
心跳仅 web、M3)。

**遗留事项(有意未做,勿当缺陷报)**:见 §9 M2 ①③ 条内"遗留"注记与 DEVLOG
"没做完/遗留"段。
