# 网络功能规格(web 上位机 / OTA / NTP / mDNS)

> 前提:WiFi 暂不可用(见 DEV_HANDBOOK §8),一切以 eth0/eth1 有线为准;wlan0 接口预留。

## 1. web 上位机(HTTP + WebSocket)

- 板上起内嵌 HTTP 服务(civetweb,单库 vendored;端口默认 **8080**,进 device_config
  `web_port`)。页面与资源内嵌在固件里(`modules/net/web/pages/` → `gen_pages.sh` →
  `web_pages.c`,生成物入库,改页面不需要前端工具链)
- 路由(方法严格校验,改状态接口只接受 POST,不符回 405):

  | 方法 | 路径 | 鉴权 | 说明 |
  |---|---|---|---|
  | GET | `/` `/app.css` `/app.js` `/favicon.ico` | 否 | 单页应用与资源 |
  | POST | `/api/login` | 否 | 账号+口令 → `{token,expires_in,user,pwd_default}` |
  | POST | `/api/logout` | token | 注销当前 token |
  | GET | `/api/device` | token | 版本(固件 git describe)/运行时长(读 `/proc/uptime`)/用户数/日志数/IP/接口名/mDNS 名与地址/账号/默认口令标记/NTP 状态/库占用与分区余量 |
  | GET | `/api/logs` | token | `from,to,user_id,page,page_size(≤100)` 分页 JSON,字段与 access_logs 一致 |
  | POST | `/api/ntp` | token | 异步触发(202),结果经 WebSocket 回 |
  | POST | `/api/account` | token + 旧口令 | 改账号/口令,成功后**吊销全部会话** |
  | POST | `/api/ota/upload` | token | 流式收包 + sha256 校验(§2) |
  | GET | `/api/ws` | `?token=` | WebSocket 推送 |

- **实时门禁状态**:WebSocket 推送每次验证事件(时间/ID/姓名/方式/结果,与 access_logs 字段一致)
  与 NTP 结果。推送由服务端独立线程主动发出——**不依赖客户端轮询**(原"客户端每 2s 发 ping
  才排水"的方案已废弃,安全性靠连接表锁 + civetweb 连接锁保证,见 web 模块 README)
- **账号管理**:
  - 设备菜单 → 设备管理 → **Web 管理**:显示服务状态与局域网访问地址、当前账号,
    可改账号 / 改口令(口令掩码输入 + 二次确认;账号与口令一起保存)
  - 上位机 → 账号安全:改账号/口令需**旧口令**,成功后前端强制重新登录
  - 凭据存 device_config(账号明文 + PBKDF2 salt/hash),**首启默认 admin/admin 并置
    默认口令标记**,UI 与上位机均提示尽快修改;账号/口令合法性走 `proto/valid.h` 同一份规则
  - 登录风控:同一来源连错 5 次锁定 60 秒(429 + Retry-After),锁定表内存维护、重启解锁
- 监控视频实时推流:复用 capture_service 帧(RTSP 拉流或 HTTP-MJPEG,取实现成本低者,
  不另开摄像头链路)——**当前仍是占位**,页面已留位
- 安全:除登录与静态资源外全部校验 token(X-Auth-Token;WebSocket 因浏览器无法加头用
  `?token=`);未授权 WS 连接显式回 401 再拒;口令走 PBKDF2 同款
- mDNS:局域网通告 `<host>.local`(A 记录)并公告 `_http._tcp` 服务(PTR/SRV/TXT),
  host 取 device_config `mdns_host`(默认 `doorguard`)。实现在
  `modules/net/mdns/`(自实现,avahi 不在 rootfs),细节见该模块 README:
  探测防重名 → 通告 → 应答(组播/legacy 单播)→ IP 变化重通告 → 关机 goodbye

## 2. OTA 远程升级(A/B 分区,应用层 HTTP 流式)

目标:主机侧脚本即可完成升级,板子校验后自动重启,失败可回退。

### 2.1 分区方案(前置设计任务)

当前 parameter.txt 只有单一 rootfs(grow),**无 A/B 无 recovery**。需先改分区规划(方案先行,
与 PROJECT_PLAN 固件 Phase 联动),二选一:

- **方案 A(推荐)**:应用级 A/B —— door-guard 应用+其依赖放独立 `app_a`/`app_b` 只读分区,
  rootfs 不动;boot 切换用 uboot env(`boot_app` A/B + `bootcount` 限次回退)
- **方案 B**:rootfs 级 A/B —— 改动大(两份 rootfs 空间、uboot altbootcmd),留作后续

### 2.2 升级流程

```
主机脚本 dg_ota_upload.sh <板IP> <升级包>
  → POST /ota/upload 流式上传(边收边写目标 B 分区,不落 tmp,控内存)
  → 板收完:校验 manifest.json 里 sha256/大小/版本号,不符即弃并报错
  → 置 uboot env:boot_app=b、upgrade_ok=0、bootcount=0,自动重启
  → 新分区启动后应用上报"升级成功"→ upgrade_ok=1;
    若 bootcount 超限仍无上报 → uboot 自动切回 A(回滚)
升级包格式:tar.gz{ manifest.json(version, sha256, size, 分区名), payload }
```

- 板上监听是 door-guard 应用内线程(不是独立守护),端口默认 **9000**(device_config)
- 主机脚本放仓库 `env/bin/dg-ota-upload`(bash+curl,`--progress-bar` 流式)
- 升级前后动作:版本号写 access/设备信息接口,web 上位机可见当前版本与 A/B 槽位

### 2.3 校验清单(实现自测)

- [ ] 断电中途:写坏 B 分区,A 启动不受影响
- [ ] sha256 不符:拒写/拒切,返回明确错误
- [ ] 升级包比 B 分区大:开始前预检大小即拒绝
- [ ] 升级中其它接口仍可用(上传不阻塞验证业务,低优先级 IO)

## 3. NTP 时间校正

触发时机(三种,全部要求设备已联网):

1. **开机自动一次**:后台服务启动即检查 eth0/eth1/wlan0 是否拿到 IP 且外网可达
   (netlink 监听 + 连通性探测);满足则执行一次,失败不重试到死(记日志)
2. 菜单页 → 设备管理 → **NTP 时间矫正** 按钮手动触发
3. web 上位机 → 设备管理 → **NTP 时间矫正** 按钮手动触发

实现:rootfs 已带 chrony(后台常驻);"执行一次"= `chronyc burst 4/4 && chronyc waitsync 10`
(或 equivalent),UI/上位机回显成功失败与当前时间;服务器地址 `ntp_server` 进 device_config。
未联网时按钮置灰并提示"设备未联网"。

装配要求(**踩过的坑**):`ntp_service_start()` 必须在 main 的 holder 表里注册。
只 include 头不初始化时 `s_running=false`,触发请求会被静默丢弃——表现是"按钮点了没反应",
既不报错也没有结果事件。上位机侧的触发是异步的(POST 立即回 202,结果走 WebSocket),
因为 `chronyc waitsync` 可阻塞十余秒,不能占住 HTTP 工作线程。
