# 开发日志(DEVLOG)

> 记录约定:每次会话/每个工作日**追加**新条目(最新在最上),写清"做了什么 / 结论 / 踩了什么坑"。
> 本日志记"过程与坑",当前状态看 `DEV_HANDBOOK.md`,方案看 `PROJECT_PLAN.md`。

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
