# 网络功能规格(web 上位机 / OTA / NTP / mDNS)

> 前提:WiFi 暂不可用(见 DEV_HANDBOOK §8),一切以 eth0/eth1 有线为准;wlan0 接口预留。

## 1. web 上位机(HTTP + WebSocket)

- 板上起内嵌 HTTP 服务(mongoose 7.23,跑在 `modules/net/netcore` 统一事件循环,
  2026-09-22 起 civetweb 退役;端口默认 **80**(2026-09-27:URL 免带端口,root 绑定
  无权限问题;非 root 环境(PC 模拟器)绑定失败自动回退 **8080** 并经 `mdns_set_port()`
  同步通告),进 cur_config.json(cfg 键 `network.web_port`),
  OTA 上传端点同端口)。前端是 **Vue 3 单页应用**(`services/web/frontend/`,Vite 构建),
  产物同步到 `pages/` 再经 `gen_pages.sh` 生成资源表 `web_pages.c`,**两者都入库**:
  固件构建机不需要 node,只有改前端时才要 `./build_frontend.sh`
- 前端分层纪律(由 `tests/web/frontend_check.py` 自动检查):
  `views → stores → api → components`,组件纯展示(props/emits,不得 import store/api,
  不得直接 fetch);色值只取 `styles/tokens.css`(与设备端 `ui/theme.h` 同源)
- 前端用 hash 路由:设备端只提供固定资源表,不需要为前端路由配服务端回退;
  未知路径回落到单页应用,`/api/` 前缀才回 JSON 404
- 路由(方法严格校验,改状态接口只接受 POST,不符回 405):

  | 方法 | 路径 | 鉴权 | 说明 |
  |---|---|---|---|
  | GET | `/` `/app.css` `/app.js` `/favicon.ico` | 否 | 单页应用与资源 |
  | POST | `/api/login` | 否 | 账号+口令 → `{token,expires_in,user,pwd_default}` |
  | POST | `/api/logout` | token | 注销当前 token |
  | GET | `/api/device` | token | 版本(固件 git describe)/运行时长(读 `/proc/uptime`)/用户数/日志数/IP/接口名/mDNS 名与地址/账号/默认口令标记/NTP 状态/库占用与分区余量 |
  | GET | `/api/network` | token | 网络配置快照:`{ifname,ip,netmask,gateway,have_ip,online,mode,configured?}`——实际地址来自 net_info,**未拿到统一 `0.0.0.0`**;mode=dhcp/static(持久化配置),静态时附 `configured`(期望值) |
  | POST | `/api/network` | token | 应用网络配置:`{mode:"dhcp"}` 或 `{mode:"static",ip,netmask,gateway(可空)}`。校验(点分/连续掩码)→ 持久化 cfg → 回 **202** → 后台线程应用(net_cfg_apply,system 与 dhcpcd 协调);完成后经 WebSocket `net` 事件推新地址。改 IP 会切走本连接,前端必须提示用新地址重访 |
  | GET | `/api/logs` | token | `from,to,user_id,page,page_size(≤100)` 分页 JSON,字段与 access_logs 一致 |
  | POST | `/api/ntp` | token | 异步触发(202),结果经 WebSocket 回 |
  | POST | `/api/account` | token + 旧口令 | 改账号/口令,成功后**吊销全部会话** |
  | POST | `/api/system/reboot` | token | **远程重启**(2026-09-27):202 受理后延迟 1s 发布 `EV_SYS_REBOOT` → sysctl 服务执行(与设备端「重启设备」同一入口);重启期间门禁与上位机短暂不可用 |
  | POST | `/api/ota/upload` | token | 流式收包 + sha256 校验(§2) |
  | POST | `/api/users/face_clear` | token | 清除已录人脸(保留用户),受理制(经 enroll 服务,异步) |
  | POST | `/api/users/face_set` | token | **人脸录入/重录(2026-10-04)**:原始 JPEG 走 body(`Content-Type: image/jpeg`,服务端硬顶 512KB、SOI 前置校验),uid 走 query。受理制:`202 {seq}` → vision worker 异步提取(检测→质量闸→对齐→ArcFace,与拍摄流同口径;解压炸弹防线=先探尺寸选缩放倍率再分配缓冲)→ 查重落库(置位+头像)→ 结果经 WS `enroll` 消息(seq 配对)。前端负责降采样(canvas 重编码,最长边 ≤1024,顺带抹 EXIF);错误码 FACE_NONE/MULTI/QUALITY 对应"未检测到/多张/质量不合格" |
  | GET | `/api/ws` | `?token=` | WebSocket 推送五类消息:`auth`(验证事件)/ `enroll`(录入回执,kind/seq/ok/err/msg,2026-10-04 起)/ `ntp`(校时结果)/ `net`(地址变化,web 轮询与 5s 监视触发)/ `uptime`(运行时长,5s 周期,概览实时显示) |

- **实时门禁状态**:WebSocket 推送每次验证事件(时间/ID/姓名/方式/结果,与 access_logs 字段一致)、
  NTP 结果、网络地址变化、运行时长(5s)。总线回调入队 + netcore_post,由 loop 线程排空并逐连接
  `mg_ws_send`——**不依赖客户端轮询**,且连接只在 loop 线程被碰(连接表无锁)。
  周期推送在无 WS 客户端时跳过,不占推送队列
- **单会话策略(2026-09-22)**:web 管理页同一时刻只允许一个管理员在线——
  登录成功即吊销其余全部会话(新登录必胜,崩溃浏览器不占坑);被踢方下一个
  请求 / WS 重连得 401,由前端路由守卫送回登录页
- **账号管理**:
  - 设备菜单 → 设备管理 → **Web 管理**:显示服务状态与局域网访问地址、当前账号,
    可改账号 / 改口令(口令掩码输入 + 二次确认;账号与口令一起保存)
  - 上位机 → 账号安全:改账号/口令需**旧口令**,成功后前端强制重新登录
  - 凭据存 device_config(账号明文 + PBKDF2 salt/hash),**首启默认 admin/admin 并置
    默认口令标记**,UI 与上位机均提示尽快修改;账号/口令合法性走 `proto/valid.h` 同一份规则
  - 登录风控:同一来源连错 5 次锁定 60 秒(429 + Retry-After),锁定表内存维护、重启解锁
- 监控视频实时推流:**复用 capture 帧,经 web 服务现有 WebSocket 周期推 JPEG 快照**
  (2026-09-20 架构 v2 决议:RTSP 暂缓,后续有高帧率需求再评估)——当前仍是占位,页面已留位
- 安全:除登录与静态资源外全部校验 token(X-Auth-Token;WebSocket 因浏览器无法加头用
  `?token=`);未授权 WS 连接显式回 401 再拒;口令走 PBKDF2 同款
- mDNS:局域网通告 `<host>.local`(A 记录)并公告 `_http._tcp` 服务(PTR/SRV/TXT),
  host 取 device_config `mdns_host`(默认 `doorguard`)。实现在
  `services/mdns/`(自实现,avahi 不在 rootfs),细节见该模块 README:
  探测防重名 → 通告 → 应答(组播/legacy 单播)→ IP 变化重通告 → 关机 goodbye

## 2. OTA 远程升级(A/B 分区,应用层 HTTP 流式)

目标:主机侧脚本即可完成升级,板子校验后自动重启,失败可回退。

### 2.1 分区方案(前置设计任务)

当前 parameter.txt 只有单一 rootfs(grow),**无 A/B 无 recovery**。需先改分区规划(方案先行,
与 PROJECT_PLAN 固件 Phase 联动),二选一:

- **方案 A(推荐)**:应用级 A/B —— door-guard 应用+其依赖放独立 `app_a`/`app_b` 只读分区,
  rootfs 不动;boot 切换用 uboot env(`boot_app` A/B + `bootcount` 限次回退)
- **方案 B**:rootfs 级 A/B —— 改动大(两份 rootfs 空间、uboot altbootcmd),留作后续

### 2.2 升级流程(2026-09-22 与实现对齐:ota_service.h / web_server.c / env/bin/dg-ota-upload)

```
主机脚本 dg-ota-upload <板IP> <升级包>
  → POST /api/ota/upload 流式上传(与 web 上位机同一 mongoose 监听、同一
    端口同 web;MG_EV_HTTP_HDRS + MG_EV_READ 增量喂入,按 ota_can_accept()
    限流,64MB 包不进内存;落盘暂存 /tmp/ota_staging.bin,支持断点续传)
  → manifest 字段走 HTTP 请求头:X-OTA-Version / X-OTA-Size / X-OTA-SHA256
    (大小预检超 MAX 拒收;sha256 流式校验;不符即弃并报错)
  → 提交:校验闭环到暂存文件为止,**不写真实分区**(分区写入与 uboot env
    方案见 docs/tech/OTA_PLAN.md:boot_app=b、upgrade_ok、bootcount 回滚约定)
升级包:单文件流(镜像/固件包本体),manifest 信息由请求头携带
```

- 监听在 door-guard 应用内(不是独立守护)。`ota_port` 独立端口方案已废弃
  (2026-09-22:配置字段删除,OTA 复用 web 端口)——实现里从未起过 9000
  监听,属文档先行、实现收窄
- 极端慢盘时收包缓冲顶到 mongoose `MG_MAX_RECV_SIZE`(3MB)会显式断连,
  客户端以 X-OTA-Offset 续传重试(有界失败 + 可恢复,不阻塞事件循环)
- 主机脚本放仓库 `env/bin/dg-ota-upload`(bash+curl,`--progress-bar` 流式)
- 升级前后动作:版本号写 access/设备信息接口,web 上位机可见当前版本与 A/B 槽位

### 2.3 校验清单(实现自测)

- [ ] 断电中途:写坏 B 分区,A 启动不受影响
- [ ] sha256 不符:拒写/拒切,返回明确错误
- [ ] 升级包比 B 分区大:开始前预检大小即拒绝
- [ ] 升级中其它接口仍可用(上传不阻塞验证业务,低优先级 IO)

## 3. NTP 时间校正

触发时机(四种,全部要求设备已联网):

1. **开机自动**:后台启动即检查 eth0/eth1/wlan0;在线立即执行一次,未在线
   则每 30s 重试(上限 ~5 分钟),之后仍由"地址变化补同步"兜底
2. **地址变化补同步**:EV_NET_ADDR(DHCP 拿到地址/应用静态配置)且本次运行
   从未同步成功过 → 自动补一次(已成功过则不再跟,避免续租刷同步)
3. 菜单页 → 设备管理 → **NTP 时间矫正** 按钮手动触发
4. web 上位机 → 设备管理 → **NTP 时间矫正** 按钮手动触发

实现(2026-09-22 起为**应用内 SNTP 客户端**,mongoose `mg_sntp_connect`,跑在
netcore 统一事件循环):成功 `settimeofday` 步进写系统时间;服务器地址 `ntp_server`
进 cur_config.json(cfg 键 `network.ntp_server`)。未联网(`net_info_is_online`)时触发直接失败返回,按钮置灰提示。
**rootfs 侧需停用 chrony 常驻**(随固件 Phase 落地):两个东西同时调系统时间会打架;
chrony 是持续 slew,SNTP 是一次步进,门禁场景接受步进(时间回拨对 access_logs
展示顺序的影响以落库 ts 为准,界面查询按时间段过滤,不受影响)。

装配要求(**踩过的坑**,仍然有效):`ntp_service_start()` 必须在 main 的 holder 表里注册。
只 include 头不初始化时 `s_running=false`,触发请求会被静默丢弃——表现是"按钮点了没反应",
既不报错也没有结果事件。上位机侧的触发是异步的(POST 立即回 202,结果走 WebSocket)。

## 4. 网络配置(IP/掩码/网关,2026-09-27)

- 配置键:cur_config.json(cfg 键 `network.net_mode`("dhcp" 默认 /"static")+ `net_ip`/`net_mask`/`net_gw`)
  (点分;gw 空串 = 不下发默认路由)。**配置 = 期望值**,实际地址永远以 net_info 读取为准
- 应用:`modules/net/net_cfg`——静态先 `dhcpcd -x <if>` 释放租约并移出 dhcpcd 管理
  (否则 dhcpcd 会按租约周期覆盖手动地址),再 `ip addr add ip/plen` + `ip route replace default`;
  回 DHCP 则清残留后 `dhcpcd -n <if>`(主进程不在才补后台实例)。应用目标接口 =
  第一个非环回 UP 接口(**不要求已有地址**——静态首次应用/开机恢复恰发生在无地址时,
  net_info 的 primary 规则要求有 IPv4,那条是展示用的)
- 装配:main 服务表 `net_cfg`(依赖 config):cfg=static 才开机应用一次;
  dhcp(默认)不干预——开机配网仍由 rootfs S41dhcpcd 负责
- 展示兜底:**任何一项取不到统一显示 `0.0.0.0`**(web /api/device、/api/network、
  主页状态栏、设备管理页弹窗)。主接口无地址时 `net_info_read` 返回 DG_OK 且
  `have_ip=false`,各展示层直接渲染不再各自兜底
- **双入口**(2026-09-27 起设备端屏幕也可设置):web `POST /api/network` 与设备端
  网络配置页(`EV_NET_CFG_SET` 总线事件,UI 不直调 net_cfg)统一汇入 web_server 的
  `network_request()`:持久化 → 后台线程应用 → 应用结果回执(设备端 `EV_NET_CFG_RESULT`
  弹窗 / web 走 `EV_NET_ADDR` → WS)。静态环境(无 DHCP 服务器)严禁切 DHCP——地址
  释放后拿不回,只能人工恢复
- 实时性:web loop 内 5s 定时(net_watch)对比地址快照,变化才发布 EV_NET_ADDR →
  WebSocket `net` 事件(前端立即重取快照)+ NTP 补同步 + UI 设备页;应用静态配置的
  结果由 apply 线程直接发布同一事件(web apply_worker)。地址没变不推送(续租不刷屏)
- 设备端 UI:主页网络图标旁小字 IP(调试);设备管理页"网络配置"按钮弹窗显示当前
  接口/IP/掩码/网关/模式(只读)。**屏幕上不做地址编辑**——数字键盘配 IP 不现实,
  设置入口在上位机(网络配置卡片:DHCP/静态切换 + 三输入框 + 应用)
- 测试:`tests/test_net_info.c`(宿主):掩码转前缀判定表、请求校验、net_info_read
  兜底语义。apply() 会真改宿主网络,不进单测——推板后板上验收

## 5. MQTT 上位机通道(2026-10-04,默认关闭)

> 细则(主题表/配置键/扩展接口/重连策略)唯一事实源:`services/mqtt/README.md`;
> 本节只记规格要点,不复表。

- 定位:设备 ↔ 平台消息干道。跑 netcore 统一事件循环(mongoose `mg_mqtt`),
  与 web/OTA/NTP/mDNS 同一传输层;**默认关**(cfg `mqtt.enabled=0`),
  broker 地址(`mqtt.uri`)属部署参数
- 主题方案:`<prefix>/status`(retain 上线/LWT 离线)、`<prefix>/event/auth`
  (验证动作转发,与 access_logs 同口径)、`<prefix>/cmd/+` → `<prefix>/rsp/<name>`
  (内置 ping/status/open;open 默认拒,`mqtt.allow_remote_open` 显式授权且
  只发 EV_MQTT_CMD 交总线——开门必须走 access 流程留痕,消费端待接入)
- 扩展口:`mqtt_publish_json()`(任意业务上报,邮箱投递宁丢不堵)+
  `mqtt_cmd_register()`(命令处理器注册)+ EV_MQTT_CMD(事件式消费);
  线程契约:mg_* 只在 loop 线程,业务侧永不阻塞
- 健壮性:断线退避重连(5→60s,连上复位);keepalive 30s + 半开检测
  (3×keepalive 无入包强制重连);停服主动发 offline retain 告别
- 测试:`tests/test_mqtt.c`(宿主,测试内起最小 broker:握手/命令往返/
  上报/净化/停服告别全覆盖)

### 5.1 MQTT OTA(2026-10-05,services/ota/ota_update)

- 控制面在 MQTT,固件本体走 HTTP 直链(公告携带 url;https/chunked 显式
  拒绝,须定长直链)。主题契约见 services/ota/README.md:`ota/version`
  retain 公告 / `ota/query` 手动检查 / `ota/state` 状态可见性
- 通道侧新扩展口 `mqtt_sub_register(suffix, fn)`(非 cmd 的平台单向推送;
  重连后统一补订,retained 消息消费方容忍重复)
- 交接边界:ota_finish 校验闭环即止,S60 ota_watch 装非活动槽+原子切换+
  秒退回滚——与 web 上传(dg-ota-upload)同一暂存面
- UI:设备管理 → 关于设备(名称/版本/构建日期/最新版本/发布日期/说明;
  检查更新=MQTT 在线才可点;自动更新=cfg `network.ota_auto_update`;
  立即更新仅 AVAILABLE 且自动关时出现)
- 版本比较:点分数字前缀逐段(v 前缀/第 3 段后缀忽略)——**发版须递增
  tag 或纯 x.y.z**,git describe 的 -N-gxxx 增量不参与比较
- 测试:`tests/test_ota_update.c`(传输注入零网络依赖)+ `test_alarm.c`
  (远程报警上报,`<p>/event/alarm`,触发源待接入但出口已收口)

