# door-guard 闲时自主开发任务(完整提示词)

> 用法:整段复制给新会话即可自主推进;可分次执行,已完成 Phase 会自动跳过。

你是 RK3576 K7 人脸识别门禁项目 door-guard 的开发工程师,在 WSL 环境长时自主推进应用开发。按下方任务清单逐 Phase 完成,**每个 Phase 的【测试】与【验收】全部通过才允许进入下一 Phase**;中断后再次启动时,先查 docs/DEVLOG.md 与 git log,跳过已完成部分,从未完成的第一个 Phase 继续。

## 0. 开工(必须按序执行)

1. 加载项目 skill:`.agents/skills/door-guard-dev/SKILL.md`,按其"开工三步"执行
2. `tail -n 30 docs/DEVLOG.md`;再看 `git log --oneline -10` 确认已完成到哪个 Phase
3. 通读 skill references 全部五份(spec-database / spec-auth-business / spec-ui / spec-network / architecture)——它们是业务规格唯一权威,与任何描述冲突时以其为准
4. `source env/env.sh && dg-build -c` 确认基线编译通过,然后在 docs/DEVLOG.md 顶部写"闲时任务开工"条目,列出本次计划的 Phase 与顺序

## 1. 环境与资产速查

- 编译:`dg-build`(板上 aarch64)、`dg-build-pc`(PC 模拟器,Phase 5 建成)、`dg-test`(ctest)
- 部署:`dg-deploy -r`(推板并运行;板 **192.168.2.95** 已 SSH 免密,串口 1500000 8N1)
- 工具链:`~/dg-toolchain`(sysroot 与板上固件 20260917-B4 同源,glibc 2.38);细节 docs/tech/TOOLCHAIN.md
- ESP32 组件模板:`/home/olwhistle/dockerNow/esp32/programs/ovs`(tasker/event_bus/holder,自带测试与 port 层,映射表见 skill references/architecture.md §2)
- 固件 B4 已知事实:WiFi 不可用以太网;dropbear SSH;rootfs 带 sqlite3/lvgl/gstreamer/rockiva
- 仓库:远端 origin 已配,提交信息沿用仓库风格(feat:/fix:/docs: + 中文)

## 2. 全局纪律(违反任何一条 = 该 Phase 验收不通过)

1. **完成的定义** = 代码 + 注释(解释"为什么")+ 测试案例 + 模块 README + 使用示例;缺一不收
2. 业务参数**零魔数**:全部进 configs/device.json 或 DB device_config 表
3. UI **零裸文本**:一律 `_("原文")`;**零裸色值**:一律 theme.h token
4. 模块间只经 proto/ 消息与 event_bus 通信,禁止跨层直调;UI 层不做业务决策
5. 错误显式处理:每个可能失败的调用都有错误分支与错误码,禁止静默吞掉
6. `dg-build` 与 `dg-build-pc` **零警告**;测试不过不进下一步
7. 只在 WSL 与板子(192.168.2.95)工作:不碰 VM/SDK,deliverables/ 与 sdk-patches/ 只读
8. 需要用户决策的点(未知硬件协议等):按门禁行业惯例自行决策 + DEVLOG 留"待确认"标记后继续,不等待
9. 同一卡点超过 30 分钟:绕行并记入 DEVLOG"未完成/下一步",不空转
10. 初始化失败/中途退出都有清理路径;共享资源加锁

## 3. 任务清单

### Phase 0 基线自检
【验收】`source env/env.sh && dg-build -c` 成功;git status 干净;DEVLOG 已写开工条目。

### Phase 1 基础组件移植(event_bus → tasker → holder,顺序不可换)
【交付物】`door-guard/proto/{event_bus,tasker,holder}/`:模板实现 + pthread port;每个附 README(模板出处、port 改动点、使用示例)与 demo 文件
【测试】
- 移植模板自带 tests/test_event_bus.c、tests/test_tasker.c 接入 ctest,断言不弱化
- 新增:多线程发布订阅压测(4 线程 × 10000 事件,零丢失、无死锁,用 tsan 跑一遍)
- holder:依赖循环检测、重复注册、ERROR 态隔离(单模块失败不拖垮注册表)
【验收】`dg-test` 全绿;event_bus 先合入并被 tasker/holder 无引用(依赖最少者先行)

### Phase 2 proto 层(消息/事件/错误码/数据结构)
【交付物】`proto/err.h`(spec-database §2 全部错误码)、`proto/events.h`(EV_AUTH_RESULT 等全部事件与负载结构)、`proto/types.h`(user_rec_t / access_log_t / log_query_t,字段对齐 spec-database §1/§4)
【测试】静态断言(role/auth_flags 位宽);事件契约测试:每种事件 publish→subscribe 收到且负载逐字段相等
【验收】`dg-build` 零警告;后续 Phase 只引用本层头文件,无自定义重复结构

### Phase 3 storage(SQLite + 加密)
【交付物】spec-database §6 全部接口 + §1/§4/§5 DDL 初始化 + PBKDF2-HMAC-SHA256 与 AES-256-CTR(优先 openssl EVP,sysroot 无则自实现纯 C 并在 README 说明)+ 特征比对迭代器(供 enroll 编排查重)
【测试】(逐条进 ctest,一条不许少)
1. 添加:正常;user_id 重复→ERR_DUP_UID;IC 重复→ERR_DUP_IC;人脸查重命中→ERR_DUP_FACE;指纹同;无密码→拒绝;第 2000 个成功、第 2001 个→ERR_USER_LIMIT(边界)
2. 密码:正确/错误/用户不存在;pbkdf2 与已知测试向量对拍
3. 日志:预插 2500 条 → 时间段查询(起止边界含/不含)、按用户、分页(整除与不整除)、倒序
4. 配置 KV:set/get/默认/覆盖
5. 加密:特征加解密 roundtrip;同密码两次加盐结果不同
6. 并发:4 线程 × 2500 条日志无丢失
【验收】ctest 全绿;生成的库用 sqlite3 CLI 校验 schema 一致;模块 README 含 schema 图与错误码表

### Phase 4 配置体系
【交付物】configs/device.json 全参数化(door_open_ms、standby_timeout_s=30、face_dup_threshold=0.90、pwd_fail_lock_n=5、pwd_fail_lock_s=60、web 端口 8080、ota 端口 9000、ntp_server 等);cjson 加载器 + device_config 封装;缺键用默认、非法值回退默认并 WARN
【测试】坏 json/缺键/类型错不崩且回退默认;json 与 DB 的覆盖优先级
【验收】ctest 全绿;grep 抽查无魔数

### Phase 5 UI 框架 + PC 模拟器
【交付物】theme.h token(spec-ui §1 色值);i18n `_()` + zh-CN/en-US.json;widgets(dg_btn/dg_popup×4/dg_kbd/dg_list);page_mgr;DG_SIM=ON 的 SDL2 后端(720×1280)+ camera sim 后端(视频/图片循环);env/bin/dg-build-pc、dg-test 脚本
【测试】
- i18n 一致性(自动化):扫描源码全部 `_()` 键,断言两份 json 全覆盖,输出缺失清单——此测试不过 Phase 永不通过
- widget 冒烟:每个 widget 创建/交互日志输出
【验收】`dg-build-pc` 出 720×1280 模拟器窗口不崩;板上 `dg-build` 同过;grep 裸色值=0、`_()` 外中文 label=0

### Phase 6 三页面 + 验证状态机
【交付物】page_home(推流层+脸框 overlay+菜单/验证按钮+弹窗系统)、page_standby、page_menu+四子页(用户管理/设备管理/门禁设置/记录查询),全部按 spec-ui §3;状态机抽成纯 C 模块(可脱离 UI 用事件序列驱动)
【测试】(spec-auth-business §5 边界逐条成用例,≥15 条)
1. 普通模式命中→绿弹窗+开门事件+日志 result=0
2. 1.5s 未命中→红弹窗+reason=1;3. 黑名单→红弹窗+reason=2
4. 点验证后 match_enabled=0:注入人脸只有框、无 1:N 结果
5. ID 不存在/黑名单/auth_flags=0 → 红弹窗,不出现方式选择
6. 每子步 5s 超时回 ST_NORMAL;timer_seq 保证旧定时器失效(专项用例)
7. 管理员模式:管理员过→进菜单;非管理员红弹窗停留;5s 无脸无点击回普通
8. 密码连错 5 次锁定 60s,第 6 次直接提示锁定
9. 成功 3s 自动回普通;期间新验证请求被忽略不崩
10. 待机 30s 无脸无操作进入(测试用短值);触摸/人脸唤醒
【验收】状态机 ctest 全绿;模拟器全流程人工走通(步骤记进模块 README);`dg-deploy -r` 板上主页可显示(摄像头可 mock)

### Phase 7 服务层
【交付物】access_service(认证融合/门控/日志唯一出口)、enroll_service(录入+查重编排)、vision(接口+板上 ROCKIVA 实现+PC mock 注入"命中 user_x")、capture(接口+板上+sim)、liveness(接口占位);全接 event_bus
【测试】端到端(mock 视觉):命中已录用户→绿弹窗→door_open→日志;陌生人→红弹窗+日志;enroll 录入→查重→入库→立即可命中
【验收】模拟器端到端演示走通(README 记录步骤);板上 vision 未通则跑 mock 并在 DEVLOG 标注,不阻塞 Phase 8

### Phase 8 板上 HAL
【交付物】gpio_hal(libgpiod,开门引脚进 device.json,占位+待确认标注)、uart_hal 框架(指纹/读卡接口+mock 后端,**协议等手册,不臆造**)、camera 板上实现(复用 SDK demo 路径;打不通 mock 占位并记录)
【测试】gpio 板上 gpioset 对拍;uart mock 回环
【验收】板上门控信号可观测、UI 正常;"待硬件确认"清单(指纹/读卡协议、GPIO 编号、摄像头真实链路)登记进 DEVLOG

### Phase 9 网络功能
【交付物】内嵌 web(civetweb/mongoose):登录(token)、WebSocket 实时事件、日志查询、设备信息+NTP 按钮、视频页,蓝白风;OTA 应用侧(HTTP 流式收包+manifest sha256 校验+写分区接口+uboot env 回退约定)+ `env/bin/dg-ota-upload` 脚本 + `docs/tech/OTA_PLAN.md` A/B 分区方案(**只写方案,不改固件**);mDNS(doorguard.local);NTP(netlink 联网检测+chronyc 一次校正+三触发点)
【测试】
- web:未登录→401;登录→200;WS 收到 mock EV_AUTH_RESULT;日志查询与 db 直查一致;坏参数/坏 json→4xx 不崩
- OTA:sha256 不符拒收;超目标大小开始前拒绝;正常包写临时文件校验闭环(不真刷);中断重传状态可恢复
- NTP:断网按钮明确失败;联网后自动校正一次(日志可见)
- mDNS:WSL 解析 doorguard.local → 192.168.2.95
【验收】浏览器 http://192.168.2.95:8080 与 http://doorguard.local:8080 全功能;dg-ota-upload 闭环通过;以上均有脚本(tests/web/)

### Phase 10 集成与文档收尾
【交付物】板上端到端联调;door-guard/README 模块索引;DEV_HANDBOOK 更新;DEVLOG 总结;"待硬件确认清单"汇总
【验收】§4 总验收清单逐条自测并在 DEVLOG 打勾

## 4. 总验收清单(全部满足才算完成)

1. `dg-build`、`dg-build-pc` 零警告成功;`dg-test` 全绿(≥60 条用例)
2. 模拟器:三页面全流程可操作,中英切换正常,全部 i18n 键覆盖
3. 板上:主页/待机/菜单可用,验证状态机行为与 spec-auth-business 一致,开门信号正确,日志落库可查
4. web:登录/WS 实时事件/日志查询/NTP 按钮可用;mDNS 可解析
5. OTA:上传→校验→写目标闭环通过(真刷待分区方案落地)
6. 每个模块 README+示例齐;DEVLOG 条目齐;"待硬件确认"全部登记;全部工作已 push

## 5. 禁止事项

- 不改 deliverables/(固件镜像)、sdk-patches/、PROJECT_PLAN.md 里程碑定义
- 不把任何密码/密钥写进仓库;不删除 DEVLOG 历史条目
- 不臆造指纹/读卡器协议;不在业务/UI 代码里写 `#ifdef DG_SIM`(仅 HAL sim 后端与 main 装配允许)
- 目标机新依赖先查 sysroot 有无;SDL2 只装 PC 宿主侧(apt,记录到 DEV_HANDBOOK)
