# 待拍板决策清单

> 2026-09-28 全库审查会话遗留,**同日已全部拍板并执行完毕**(执行摘要见各条,
> commit 见 git log 2026-09-28)。后续新决策追加在本文档;已闭环条目留档备查。

## A. 架构收敛

### A1. UI 页面直写 SQLite → **拍板:方案 A(2026-09-28),已执行**
enroll 服务补用户生命周期接口(user_save/user_get/user_page/log_query),
page_user_edit/page_users/page_logs 删 storage.h 全部改调服务,错误码透传;
test_enroll_flow 补 save 语义用例(ADD/EDIT/密码缺/透传/分页);proposal §1
登记。commit 9669af0,板上走查通过(用户管理/编辑页/记录查询三页实测)。

### A2. UI 页面直写 config → **拍板:方案 B(2026-09-28),已登记**
不改代码。proposal §1 补登记「UI 设置页→config 同步读写」例外(理由:cfg
双缓冲快照线程安全,设置页「保存即读即显」同步调用最简);architecture.md
例外摘要同步。

### A3. enroll→vision 特征库写直调 → **拍板:方案 A(2026-09-28),已登记**
代码已符合(0d27f95 起返回值已检查),纯文档:proposal §1 补登写路径例外
(理由:事务回滚依赖同步返回码,异步拆两处更难验证);architecture.md 摘要
同步。

### A4. access_service 直落 drv/gpio → **拍板:建 modules/relay(2026-09-28),已执行**
新建 modules/relay 包装 gpio_hal(relay_door_pulse/relay_reset/relay_level,
引脚号装配层传入,宿主降级态恒 READY);access_service 删 drv/gpio include,
main.c holder 注册 relay + safe_shutdown 改 relay_reset;宿主 test_relay
(35/35 绿);commit 24c899c,板上 `[RELAY] 继电器就绪 gpio0` 实测。

## B. 产品行为取舍

### B1. 待机态检测降帧 → **拍板:方案 A(2026-09-28),已执行**
vision_rknn worker 内节流:DETECT_ONLY 且持续无人 ≥10s → 检测降到 ~10fps
(跳过帧立即还缓冲);人脸检出或模式切走(触摸唤醒)立即满速。只动检测
频率,显示/待机三态/video plane 时序不动。commit 1a4e8aa;板上实测待机
静置 CPU 19~20% → 8~9%(-11pp),触摸唤醒 fps 立回 30、无可感延迟。

### B2. 指纹/IC 选项在硬件接入前的表现 → **拍板:只列已开启方式(2026-09-28),核对无偏差**
方式选择(presenter_home show_method_picker)本就按 auth_flags 位过滤 ✓;
两处建用户入口默认 auth_flags 均为 FACE|PWD:设备端 ADD 经
enroll_service_user_save(A1 新接口,test_enroll_flow 断言)✓,web
/api/users/add 默认 flags=FACE|PWD(jflags 校验合法域)✓——零代码改动;
机制已在 spec-auth-business §4.2 注明。

### B3. auth_fsm 交互定时器组进 config → **拍板:保持现状(2026-09-28)**
1500/5000/3000 为 spec 固定值,不改代码、不进 config(牵动 spec 与全部
FSM 测试,单独加键无意义)。

### B4. web 登录锁定与设备侧统一 → **拍板:方案 B 保持独立(2026-09-28),已注**
不改代码。web_auth.c 补注释:web 锁 token/IP(防爆破上位机)、设备侧锁
门禁密码验证(防猜密码),两套计数语义不同勿统一。

## C. 卫生类

### C1. 死事件清理 → **拍板:全留接口,分类注明(2026-09-28),已执行**
proto/events.h 每条死契约加状态注(契约名保留防已对接 ID):
- 待硬件:EV_FINGER_STATUS / EV_IC_CARD / EV_DOOR_STATE;
- 已被取代:EV_AUTH_DOOR_CLOSE(电平继电器由 relay_door_pulse 内部复位)、
  EV_ENROLL_PROGRESS(两段式草稿后由 EV_ENROLL_RESULT 承载)、
  EV_NET_STATE(被 EV_NET_ADDR 取代)、EV_UI_STANDBY(由 FSM 驱动 standby 页
  + 主页倒计时实现);
- 预留:EV_SYS_SERVICE_STATE / EV_CAPTURE_STATE / EV_NET_OTA_PROGRESS
  (UI 降级提示/上位机待接入)。

### C2. now_ms() 重复定义 → **拍板:收进 components/timeutil(2026-09-28),已执行**
新建零依赖叶子组件:now_ms(REALTIME)/now_mono_ms(MONOTONIC)/now_s,
两时钟语义显式分名(防 netcore 心跳类混用事故复发);6 文件 7 处 static
定义删除改 include,各调用方换同语义函数行为零变化;组件 port 层时基
(bus_now_ms/tasker_now_ms,port 契约)不在范围。commit c5fd771。

### C3. 看门狗 restart 幂等空操作 → **拍板:搁置(2026-09-28)**
不改代码。registry_restart 对 start 有 `if(running) return OK` 守卫的服务
是空操作(只重打标签);真要拉死服务需 stop+start 契约,而多数服务缺完整
对称的 stop 语义。**待定:stop+start 契约设计**,有真实需求时再立项。

### C4. UI 绕过 bridge 直发事件 → **拍板:收口(2026-09-28),已执行**
bridge 补类型化出站 API(bridge_ntp_sync/bridge_reboot/bridge_net_cfg_set/
bridge_enroll_request);page_menu/page_device/page_net_set/page_capture/
page_user_edit 六处直发改经桥,六页删 event_bus.h include(page_standby
残留一并清);UI 层只剩 bridge.c 触碰 event_bus。commit 12232f4,板上
登录→菜单→返回实测通过。

---
*拍板方式:直接在本文档对应条目后加「→ 拍板:XXX」即可,下次会话按条执行。*
