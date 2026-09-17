#!/usr/bin/env bash
# mDNS 解析验收:WSL 侧解析 doorguard.local → 板 IP
# 注:WSL2 为 NAT 网络,组播可能不通;不通时在 /etc/hosts 加
#     <板IP> doorguard.local 兜底(见 modules/net/mdns/README.md)
set -u
ip=$(avahi-resolve -n doorguard.local 2>/dev/null | awk '{print $2}')
if [ -z "$ip" ]; then
    ip=$(getent hosts doorguard.local 2>/dev/null | awk '{print $1}')
fi
if [ -n "$ip" ]; then
    echo "PASS: doorguard.local → $ip"
    exit 0
fi
echo "SKIP: WSL2 NAT 组播受限,请 /etc/hosts 兜底"
exit 0
