# mdns_responder — doorguard.local 通告

轻量自实现(avahi 不在 B4 rootfs):监听 224.0.0.251:5353,对
`doorguard.local` 的 A 记录查询回本机 IP(首个非 lo IPv4)。

## WSL2 说明

WSL2 是 NAT 网络,组播/单播 mdns 从 WSL 到 LAN 可能不可达。验收:
- 板上(或同网段 Linux):`avahi-resolve -n doorguard.local` → 192.168.2.95
- WSL 兜底:`/etc/hosts` 加 `192.168.2.95 doorguard.local`

脚本:`tests/web/mdns_test.sh`(仓库 door-guard/tests/web/)。
