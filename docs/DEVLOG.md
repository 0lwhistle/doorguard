# 开发日志(DEVLOG)

> 记录约定:每次会话/每个工作日**追加**新条目(最新在最上),写清"做了什么 / 结论 / 踩了什么坑"。
> 本日志记"过程与坑",当前状态看 `DEV_HANDBOOK.md`,方案看 `PROJECT_PLAN.md`。

---

## 2026-09-18 Phase 8 板上 HAL 完成 + 板失联事故

### 完成内容
- **gpio_hal**:B4 rootfs 无 libgpiod/gpiod CLI(实测)→ sysfs 实现;
  引脚号入 device.json(access.relay_gpio_line);开门脉冲+电平读回对拍接口
- **uart_hal**:termios 框架(原始模式+接收线程)+ mock 回环后端;
  test_uart_mock 回环三帧/重复打开拒绝/关闭后拒发全绿;
  **协议纪律执行**:指纹/读卡帧协议等手册,auth/{finger,card} 层未动
- **camera_board**:实测探测到 /dev/video0-72(rkisp 多节点)——B6/B7
  ISP 链路定位的重要线索;真实取流仍 mock 占位
- access_service OPEN_DOOR 接 gpio_hal(初始化失败降级为仅事件可观测)

### ⚠️ 事故记录(引以为戒)
- 板上 gpio 对拍时选择了未知引脚 gpio108,写 direction 后**板上内核挂起
  失联**(ping 不通),需**物理断电恢复**;door-guard 程序本身无恙
- 教训:GPIO 物理操作必须先确认引脚复用状态(pinctrl),未知引脚禁止写
  direction。gpio_hal 代码路径正确性待引脚确认后重测

### 待硬件确认清单(累计)
1. 继电器接线引脚(access.relay_gpio_line)与继电器类型(电平/脉冲)
2. 指纹模块型号/协议/接线 UART
3. IC 读卡器型号/协议/接线
4. 摄像头真实链路:rkaiq 3A + V4L2(B6/B7,/dev/video0-72 已探明)
5. 触摸:GT9xx 待 B5 固件(B4 FTS probe fail 无输入节点)

### 未完成 / 下一步
- Phase 9 网络功能(web/OTA/NTP/mDNS)

---

## 2026-09-18 Phase 7 服务层完成

### 完成内容
- **access_service**:FSM 唯一持有者;日志落库与 EV_AUTH_RESULT 唯一出口;门控事件;
  UID 解析/密码验证(连错预检)服务侧完成;FSM 定时器经 tasker 回注总线(单线程语义)
- **vision_service**:特征槽位句柄模式(8 槽环形,明文即取即清,大数据不过总线);
  sim mock(周期事件+确定性伪特征)+ rockiva 占位(B7/B8)
- **enroll_service**:录入编排(请求→抓取→查重→入库→回执);**capture_service**
  相机状态巡检广播;**liveness_service** 占位
- events.h 扩展 UI↔服务契约(EV_UI_BTN/TEXT_INPUT/METHOD_PICK/TOUCH/GOTO_PAGE/HINT);
  page_home 瘦身为纯渲染+事件转发
- test_e2e:事件总线全链路——录入→查重→入库→可命中;命中→开门+日志 result=0;
  陌生人 1.5s→失败 reason=1;密码错/对两路径;14/14 常规+tsan 全绿,三端零警告

### 坑
- tsan 连抓两处:FSM 心跳在 tasker 线程直接驱动(改经总线回注);
  event_bus `initialized` 普通 bool 与在途 publish 竞争(改 C11 原子)
- enroll 的 db_user_update 覆盖语义会清 role/auth_flags——编排侧先取旧记录回填
  (教训:覆盖式 update 调用方必须带全量字段)

### 未完成 / 下一步
- Phase 8 板上 HAL(gpio_hal/uart_hal 框架/camera 板上链路)

---

## 2026-09-18 Phase 6 三页面 + 验证状态机完成

### 完成内容
- **auth_fsm**:纯 C 事件驱动状态机,事件注入+动作回调,不碰 LVGL/DB;
  timer_seq/密码连错锁定/黑名单/日志唯一出口逐条实现;test_auth_fsm 11 用例
  40+ 断言覆盖 spec-auth-business §5 全部 10 组边界
- **七页面**:home(推流 canvas+黄/绿/红脸框+菜单验证按钮+四类弹窗)/
  standby(黑屏 HH:MM)/menu(四宫格)/users/device/access_set/logs,
  语言切换经 EVENT_UI_REFRESH_REQUEST 全页重建刷新
- **板上显示打通**:rockchipdrmfb(/dev/fb0)mmap 返回 EBUSY(实测,仿真层
  限制)→ 改走 lv_drivers DRM dumb-buffer(libdrm);开机 lv_demo 占用 fb
  需停用(已 mv disabled);板上主页渲染截图 docs/img/board-home-phase6.png
- **模拟器**:七页面全部渲染验证;DG_SIM_VISION=0 可关视觉 mock 交互走查;
  全流程截图(输入弹窗/键盘/成功失败弹窗/待机)

### 待确认(硬件)
- 板上触摸:B4 固件无 GT9xx/FTS 输入节点(FTS probe fail),待 B5 固件;
  板上 UI 当前只显示无触摸
- 字体注记:DroidSansFallbackFull 无 ASCII 字形 → 字体生成脚本改双字体
  (DejaVu Latin + Droid CJK),gen.sh 已固化

### 坑
- `source env/env.sh` 必须在仓库根执行,子目录静默失败导致几轮"板上没跑
  新二进制"的假象(md5 校验才定位到)
- DroidSansFallback 无 Latin → 数字全方块;lv_font_conv 多 --font 段解决
- 板上 lv_demo 占用 fb/DRM,door-guard 启动前须停(已禁自启,待 B10 rootfs 收编)

### 未完成 / 下一步
- Phase 7 服务层(access/enroll/vision/capture/liveness,全接 event_bus)

---

## 2026-09-18 Phase 4 配置体系 + Phase 5 UI 框架/PC 模拟器完成

### 完成内容
- **Phase 4**:cJSON vendored(third_party/cjson,MIT);config/cfg 三层覆盖
  (默认→device.json→DB device_config),类型错/越界 WARN 回退不崩;cfg_set 校验+
  持久化;test_cfg 全绿;device.json 补齐任务要求业务键
- **Phase 5**:LVGL 8.3 vendored 双端同源编译(自定义 lv_conf:CJK 字体+256KB 池);
  theme token / i18n(_()+双 json+常驻表缓存)/ widgets(dg_btn/dg_popup×4/dg_kbd/
  dg_list)/ page_mgr / 自定义中文字体(gen.sh 从 lang 字符集生成);
  hal/display sim=SDL2 自写驱动 720×1280,hal/camera sim=stb_image 图片循环;
  dg-build-pc 脚本;test_i18n(键覆盖+裸中文=0+字形覆盖)+ test_widgets(无头冒烟)
- **验收达成**:模拟器 720×1280 稳定运行,截图 docs/img/sim-phase5-widgets.png;
  dg-build 交叉零警告;dg-test 12/12(常规+tsan)全绿;裸色值=0、裸中文 label=0(自动化)

### 踩坑记录
- **LVGL lv_conf.h 模板整体包在 `#if 0`**:忘改 `#if 1` 导致全部配置不生效
  (字体缺符号/定时器异常),pragma message 是线索
- **include 守卫与 token 同名**:theme.h 的 `#define DG_BTN_H 96`(按钮高度)撞上
  dg_btn.h 的守卫 DG_BTN_H,头文件被整体跳过 → 隐式声明 → 指针截断段错。
  守卫一律加模块前缀(DG_WIDGETS_BTN_H)
- **静态库符号环**:lv_conf LV_TICK_CUSTOM 回引 ui/port.c 而部分 lvgl 成员被 ui 引用,
  单遍 ld 解析不了 → 应用链接用 `-Wl,--start-group/--end-group`
- **UTF-8 三字节解码掩码**:第二字节是 0x3F 不是 0x1F(复制 2 字节分支的笔误)
- tests 配置分支若在目标定义前 return,后面的库对测试不可见——分支必须放最后
- 字体策略:内置 SIMSUN_16_CJK 是日文/繁体字集(简体几乎全缺),必须自生成;
  宿主已有 node(lv_font_conv),字体 .c 入库使测试机免 node

### 未完成 / 下一步
- Phase 6 三页面 + 验证状态机(spec-auth-business §5 边界 ≥15 条用例)

---

## 2026-09-18 Phase 2 proto 层 + Phase 3 storage 完成

### 完成内容
- **Phase 2**:`proto/{err.h,types.h,events.h}` 统一契约。错误码分段(通用/用户/验证/网络);
  user_rec_t/access_log_t/log_query_t 字段对齐 spec-database,role/auth_flags 位宽
  _Static_assert;18 种 EV_* 事件+负载(编译期 ≤256B 守卫)+ 事件契约测试(发布→订阅
  回捕逐字段相等 + EV_AUTH_RESULT 显式比对)。proto/README 含使用示例
- **Phase 3**:`hal/storage`(storage.c/crypto.c)。DDL 与 spec 逐字一致;加密走 sysroot
  openssl3 EVP(PBKDF2-HMAC-SHA256 10000 iters / AES-256-CTR 随机 IV 前缀+设备密钥 dg.key);
  添加全链路校验(密码必填/uid/IC/特征查重/2000 上限);特征查重用比较器注入
  (`storage_set_feature_cmp`,基线逐字节,Phase 7 注入 ROCKIVA 相似度);日志查询
  时间段含边界/按用户/分页/倒序;配置 KV upsert;特征迭代器供 enroll 编排
- 验收:sqlite3 CLI 校验 schema 一致;库文件/密钥 0600;dg-test 9/9(常规+tsan)全绿;
  dg-build 零警告;宿主 apt 装 libsqlite3-dev(测试构建用,记 DEV_HANDBOOK)

### 踩坑记录
- **LIMIT ?N 动态占位符错位**:`LIMIT ?4` 在无 WHERE 时 ?4 未绑定 → SQLite 视为 NULL
  → 0 行返回。改为已校验整数内联 LIMIT/OFFSET,过滤值保持绑定
- **测试数据算术**:seed 里 i%50==0 的行同时 i%5==0(陌生人→user_id NULL),"按 U000
  查 50 条"永远查不到;同理 i=4 不是陌生人。教训:测试数据生成器要和断言一起推演
- tsan 连抓三处测试代码竞争(DG_CHECK 全局计数被 handler 线程改)——测试代码也要原子纪律

### 未完成 / 下一步
- Phase 4 配置体系(configs/device.json 全参数化 + cjson 加载器)

---

## 2026-09-18 Phase 1 基础组件移植完成(event_bus → tasker → holder)

### 完成内容
- `proto/dg_log`:组件共用极简日志(承接模板 logger,后续统一日志模块只换实现)
- `proto/event_bus`:pthread port;锁外回调/事件池+堆兜底/原子统计保留;新增
  dg_event_pool 自实现定长块池、分发任务 stop+join 清理路径;模板测试 EB1~EB5
  断言未弱化;新增 4×10000 压测(载荷 (tid,seq) 恰好一次 = 零丢失)+ tsan 全绿
- `proto/tasker`:pthread port;5.2 三修复与自旋熔断逐行保留;**tsan 检出模板固有
  数据竞争**(跨线程标志非原子 + is_empty/is_full 无锁读),统一改 C11 原子访问 +
  补调度表锁,调度逻辑不变,TSAN 复跑全绿
- `proto/holder`:pthread timedlock 等价带超时取锁;新增 test_holder 61 断言
  (循环依赖/重复注册/ERROR 态隔离/必需模块停机)
- `dg-test` 脚本建成(宿主 gcc + ctest,`--tsan` 可选);7/7 常规全绿、tsan 全绿;
  dg-build 交叉编译零警告;demo×3 全部可执行并进 ctest 冒烟

### 结论 / 坑
- **压测 drop 计数语义**:event_bus 的 `events_dropped` 是"队列满丢弃的发布尝试
  次数",发布方按契约重试后事件不丢;零丢失须由载荷序号恰达一次证明,不能断言 drop==0
- 模板质量总体高,但"5.2 修复版"在 tsan 下仍有竞争——移植不是复制,并发组件必须跑 tsan

### 未完成 / 下一步
- Phase 2 proto 层(err.h/events.h/types.h + 静态断言 + 事件契约测试)

---

## 2026-09-18(闲时任务开工)10 Phase 应用开发启动

### 完成内容
- 按开工三步恢复上下文:git log 确认全部 Phase 未开始,从 Phase 0 起步
- 通读 skill references 全部五份(spec-database / spec-auth-business / spec-ui / spec-network / architecture)
- Phase 0 基线自检:`source env/env.sh && dg-build -c` 成功,git status 干净

### 本次计划 Phase 顺序
- Phase 1 基础组件移植(event_bus → tasker → holder)
- Phase 2 proto 层 → Phase 3 storage → Phase 4 配置体系
- Phase 5 UI 框架 + PC 模拟器 → Phase 6 三页面 + 验证状态机
- Phase 7 服务层 → Phase 8 板上 HAL → Phase 9 网络功能 → Phase 10 集成收尾
- 每 Phase 测试与验收全过才进下一 Phase;卡点 >30min 绕行并记 DEVLOG

---

## 2026-09-17(晚)目录整理 + door-guard-dev skill + 环境资产落库

### 完成内容
- 目录重构:`docs/`(DEVLOG/TECH 文档)、`env/`(env.sh + dg-build/dg-deploy/dg-tc-install/dg-serial,source 后免路径);FLASH_GUIDE 迁至 docs/tech/FLASHING 并修正 IDB/分区表错误(parameter 0x800→0x0)
- WSL 工具链:sysroot 坏包已由 VM 重导(软链未解引用),sqlite 探针通过;cmake 工具链文件适配真实目录名+多用户探测
- 创建 `.agents/skills/door-guard-dev/`:SKILL.md 工作流 + 5 份 references(数据库/认证业务/UI/网络/架构),全量业务规格入库
- ESP32 模板(/home/olwhistle/dockerNow/esp32/programs/ovs)API 已确认(tasker/event_bus/holder,带 port 层),映射表写入 skill references/architecture.md
- 环境资产落库:板 IP 192.168.2.95(env.sh 默认)、root 口令、Gitea 地址、模板路径 → DEV_HANDBOOK §4/§6
- 板子已装 WSL 公钥免密;`dg-deploy -r` 零参数推板运行验证通过

### 未完成 / 下一步
- 移植模板组件:event_bus → tasker → holder(按 skill architecture.md §2.2 纪律)
- 主页面验证状态机、用户管理页、OTA 分区方案(对照 references 规格)
- WiFi 驱动修复、recovery、ISP 节点定位(B6)仍挂账

---

## 2026-09-17 首版固件产出 + 烧录上板 + WSL 交叉编译链路打通

### 固件(B1~B4,VM 侧)

- SDK 解压(19GB)并通过 `git fsck --full` 全量校验,建 `k7-door-guard-dev` 分支
- Buildroot doorGuard 定制配置(无桌面,LVGL+DRM、ROCKIVA、RKADK+RKAIQ、rknpu2、
  GStreamer+RTSP、SQLite、dropbear/chrony),5 寸屏 F050008M01 使能
- 产出 **20260917-B4** 首版固件(分区五件套),后补 `update.img` 一键包(488MB,recovery 空)
- **已知缺口**:WiFi(SWT6621S)驱动编译失败(厂家脚本头文件拷贝顺序 bug),联网用以太网;recovery 延后

### 烧录(坑:IDB 保留区)

- **坑**:分区烧录时按老平台习惯把 parameter.txt 填 0x800 扇区,被 RKDevTool 拦截:
  "IDB 将会被 parameter.txt 破坏,不要写数据在 4824 扇区之前"
- **结论**:RK3576 的 eMMC 前 8MB(0x4000 扇区前)是 BootROM/Loader 保留区;
  Loader 与 parameter 两行都填 **0x0**(工具特殊处理),其余分区镜像 ≥0x4000
- 实际采用 update.img 一键烧录成功,Loader 刷坏也可走 Maskrom 救回(不依赖 eMMC)
- 详见 `docs/tech/FLASHING.md`

### B5 验收进展

- ✅ 串口登录(1500000 8N1)、屏幕点亮 + GT9xx 触摸、IMX415 出流(cam2,3864×2192 RAW10)
- ⏳ ISP 节点定位 / NV12 抓帧(B6)、rknpu 驱动确认
- **坑**:以太网开机不自动配置——rootfs 装了 dhcpcd 但没有开机脚本拉起,`eth0/eth1`
  停在 `state DOWN`(管理关闭,不是硬件问题);手动 `ip link set eth0 up` + `dhcpcd eth0` 即通
- ✅ dropbear SSH 可登录(root;dropbear 拒绝空密码,先串口 `passwd root`)

### WSL 交叉编译环境

- 从仓库 `deliverables/wsl-toolchain/` 安装:gcc-arm-10.3(aarch64-none-linux-gnu)+
  doorguard sysroot(B4 buildroot staging,glibc 2.38,与板上 .so 严格同源)
- **坑**:sysroot 首次打包误把 staging **符号链接**直接打进 tar(276 字节坏包),
  必须解引用(`tar czfh`)或 `-C` 进实体目录归档;已重导修复,命令见 `docs/tech/TOOLCHAIN.md`
- **坑**:gcc 包解压后目录名带版本号(`gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu`),
  与文档不符;`cmake/aarch64.cmake` 已改为自动探测 + sysroot 三级回退
- sqlite3 探针编译链接通过(NEEDED libsqlite3.so.0,B4 rootfs 自带)
- ✅ **冒烟程序板上运行通过**:`arch=aarch64 kernel=6.1.75`,"WSL 写码 → dg-build → dg-deploy → 板上跑"全链路打通

### 下一步

1. 开机自动配网(脚本化 dhcpcd 自启,进 buildroot overlay,下一版固件带上)
2. B6:ISP 节点定位 + rkaiq 3A + NV12 抓帧;B7:ROCKIVA 上板
3. WiFi 驱动修复、recovery 补齐(不阻塞)
