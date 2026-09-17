# 网络功能规格(web 上位机 / OTA / NTP / mDNS)

> 前提:WiFi 暂不可用(见 DEV_HANDBOOK §8),一切以 eth0/eth1 有线为准;wlan0 接口预留。

## 1. web 上位机(HTTP + WebSocket)

- 板上起内嵌 HTTP 服务(建议 civetweb/mongoose 单库接入 buildroot 包;端口默认 **8080**,
  进 device_config)
- 功能:
  | 功能 | 说明 |
  |---|---|
  | 用户登录 | 账号+密码(管理员凭据,存 device_config/独立表,首次开机默认口令强制修改);会话用随机 token,超时重登 |
  | 实时门禁状态 | WebSocket 推送:每次验证事件(时间/ID/姓名/方式/结果,与 access_logs 字段一致)、设备在线状态 |
  | 监控视频实时推流 | 复用 SDK GStreamer:RTSP 拉流地址经 web 页内嵌播放,或 HTTP-MJPEG 直出(实现取投入产出比高者,统一走 capture_service 帧,不另开摄像头链路) |
  | 查询门禁信息 | 按时间段+用户查 access_logs(复用 storage HAL 接口),分页 JSON |
  | 设备管理 | NTP 时间矫正按钮(见 §3);只读设备信息(版本/运行时长/存储占用);用户管理暂不开放 web 端,接口预留 |
- 安全:登录才能访问(除登录页);口令走 PBKDF2 同款;所有下行接口校验 token
- mDNS:局域网通告 `doorguard.local`(`_http._tcp`,端口 8080);实现用 avahi(buildroot 包)
  或轻量 mdnsd,开机自启

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
