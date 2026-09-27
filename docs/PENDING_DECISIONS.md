# 待拍板决策清单

> 2026-09-28 全库审查会话遗留。以下事项**没有当场重构**(风险大或属产品取舍),
> 每条给出现状、候选方案与建议,等你拍板后执行。已当场修复的安全/业务缺陷
> 见 DEVLOG 2026-09-28 条目(bd7f05b / 0d27f95)。

## A. 架构收敛(重构量级)

### A1. UI 页面直写 SQLite(越层,写路径未登记)
- **现状**:`ui/pages/page_user_edit.c` 直调 `db_user_add/update/set_password`
  (保存/建用户);`page_users.c`/`page_logs.c` 直调 `db_user_*`/`db_log_query`(读)。
  架构白名单只登记了 dg_avatar 的头像**读**。
- **方案 A(建议)**:enroll 服务事实上已是"用户生命周期服务"(草稿/删除/清脸
  都在它那),补 `enroll_service_user_save()`(字段+密码一次落库)与
  `enroll_service_user_page()`(分页读),编辑页/列表页改调服务;改动集中
  两个页面 + 服务,测试有 test_enroll_flow 兜底。
- **方案 B**:在 architecture-v2-proposal §1 登记为"UI→sqlite 读写例外",
  承认现状(最快,但白名单形同虚设)。
- **方案 C**:全部走事件总线(UI 发命令、服务回执)——最纯,但保存的同步
  错误回显(弹窗文案按错误码映射)要走异步回执,UI 改动最大。

### A2. UI 页面直写 config(cfg_set_*,共 4 页)
- **现状**:page_access_set/page_device/page_face_set 直接 `cfg_set_*` 落盘。
  UI→services 白名单只许 proto。
- **方案 A**:发 `EV_CFG_SET` 事件由 config 服务执行(需新增事件+同步回执,
  config 服务现在没有命令入口)。
- **方案 B(建议)**:登记"UI 对 config 的同步读写例外"——cfg 双缓冲快照本身
  线程安全,页面保存后即读即显的交互用同步调用最简单;把例外写进白名单
  比造假事件总线更诚实。

### A3. enroll→vision 特征库写直调(services 间,写路径未登记)
- **现状**:commit/clear/delete 直调 `vision_service_library_add/remove`
  (2026-09-28 事务化后返回值已被检查)。architecture.md 摘要只写"只读直调
  两条",与事实不符(文档已按现状改,但登记制层面要拍板)。
- **方案 A(建议)**:登记为写路径例外(理由:事务序需要同步返回码做回滚,
  走总线异步回执会把回滚逻辑拆到两处,更难验证)。
- **方案 B**:改 `EV_VISION_LIB_ADD/DEL` 事件 + 同步等待回执(总线目前无
  同步请求原语,要先加)。

### A4. access_service 直接落 drv/gpio(继电器)
- **现状**:`access_service.c` include `drv/gpio/gpio_hal.h` 直驱开门脉冲。
  规划里继电器应包成 module(如 modules/relay)。
- **建议**:低优先;现直调有注释、单点、无业务逻辑泄漏。要么补登记,
  要么 B9 硬件阶段一并包 module。

## B. 产品行为取舍

### B1. 待机态检测降帧(省 CPU)
- **现状**:待机(STANDBY)vision 仍 DETECT_ONLY 逐帧检测(30fps×6.7ms),
  整机 ~21% CPU 里视觉链路占大头;门禁设备一生绝大多数时间在待机。
- **方案 A(建议)**:待机且持续无人 10s 后检测降到 10fps(唤醒延迟最多
  +200ms,触摸唤醒不受影响);CPU 预计降 6~8pp。
- **方案 B**:保持现状(响应优先)。

### B2. 指纹/IC 选项在硬件接入前的表现
- **现状**:主页"验证"→方式选择会列出指纹/IC(若用户开了该方式),选中后
   FSM 进子步等事件,5s 超时失败(reason=TIMEOUT)。EV_FINGER_STATUS/EV_IC_CARD
   事件两端皆死(驱动未接,per 2026-09-27 方案文档在等你的硬件)。
- **建议**:硬件接入前,UID 解析回执时把未接硬件的方式位过滤掉(或弹
  "模块未接入")——避免用户白等 5s。你定:过滤 or 保留入口当提示。

### B3. auth_fsm 交互定时器组进不进 config
- **现状**:1500(1:N 判定窗)/5000(子步与管理员)/3000(结果弹窗)硬编码,
  spec 写死。standby/menu/door_open 已进 config。
- **建议**:保持 spec 固定值(改这些会牵动 spec 与全部 FSM 测试);若要可调,
  连 spec 一起改,单独加 config 键没意义。

### B4. web 登录锁定策略与设备侧统一
- **现状**:web_auth.c `FAIL_LIMIT 5/LOCK 60s` 硬编码;设备侧同语义键
  `pwd_fail_lock_n/s` 在 config 可调。两处独立。
- **方案 A(建议)**:web_auth 读 cfg(两处策略一致,运维一处调)。
- **方案 B**:保持独立(web 面 token 锁定与门禁密码锁定本就是两回事)。

## C. 卫生类(小,但要你点头才动)

- **C1 死事件清理**:`EV_FINGER_STATUS/EV_IC_CARD/EV_DOOR_STATE/EV_NET_STATE/
  EV_UI_STANDBY/EV_ENROLL_PROGRESS/EV_AUTH_DOOR_CLOSE` 两端皆死(部分等硬件,
  部分是被取代);`EV_SYS_SERVICE_STATE/EV_CAPTURE_STATE/EV_NET_OTA_PROGRESS`
  有发布无订阅(OTA 进度本打算给上位机,WS 未接)。删 or 留接口等硬件?
- **C2 now_ms() 7 处重复定义**:收进 components(如 logger 附带)还是 proto?
- **C3 看门狗 restart 对幂等服务是空操作**:全部服务 start 有 `if(running)
  return OK`,registry_restart 实际只重打标签;真要拉死服务需 stop+start
  契约(现在多数服务没有 stop 语义的完整对称)。
- **C4 UI 绕过 bridge 直接 EVENT_BUS_PUBLISH**:6 处(page_menu/device/
  net_set/capture/user_edit/i18n 已改掉)。事件总线本身是 components,不算
  越层,但 bridge 的定位("UI 收发都走它")被绕开。要不要收口?

---
*拍板方式:直接在本文档对应条目后加「→ 拍板:XXX」即可,下次会话按条执行。*
