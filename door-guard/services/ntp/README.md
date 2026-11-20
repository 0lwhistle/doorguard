# ntp_service — NTP 时间校正(应用内 SNTP)

四触发点(2026-11-21 与代码对齐,spec-network §3 同口径):
1. **开机自动重试**(延迟 10s 后每 30s 重试至 ~5 分钟,`BOOT_RETRY_S=30`/`BOOT_RETRY_MAX=10`;
   `net_info_is_online` 判定,成功即停)
2. **网络地址变化补同步**(`EV_NET_ADDR`,2026-10 起)
3. 设备菜单 → 设备管理 → NTP 时间矫正(`EV_NET_NTP_TRIGGER`)
4. web 上位机 → 设备管理(`/api/ntp`,202 受理 + WebSocket 回显结果)

## 实现(2026-09-22 起)

`mg_sntp_connect`(mongoose,跑在 `modules/net/netcore` 统一事件循环)向
cfg(`cur_config.json`,DB 冻结后改走 cfg 层)的 `network.ntp_server`(默认
ntp.aliyun.com)发 SNTP 请求:

- 成功 → `settimeofday` 步进写系统时间 → `EV_NET_NTP_RESULT(ok=true)`
- DNS/网络失败(`MG_EV_ERROR`)、8s 超时 → 失败事件(不重试到死)
- 触发立即返回(`ntp_service_trigger` 非阻塞);在途请求去重由 loop 私有
  指针承担;业务线程绝不阻塞

## 为什么换掉 chrony

rootfs 需要停用 chrony 常驻(两个东西同时调系统时间会打架),随固件 Phase
落地。语义差异:chrony 持续 slew,SNTP 一次步进——门禁场景接受步进,时间
回拨对 access_logs 的影响以落库 ts 为准(查询按时间段过滤,不受影响)。

## 历史坑(仍然有效)

`ntp_service_start()` 必须真装配(main holder 表)。只 include 头不初始化时
`s_running=false`,触发请求被静默丢弃——表现"按钮点了没反应",不报错也无结果事件。
