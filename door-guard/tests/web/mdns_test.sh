#!/usr/bin/env bash
# mDNS 验收:确认设备在局域网里"按名字找得到"。
#
# 两层检查:
#   1) 报文级(必跑):向 5353 发 QU 单播查询,验 A/PTR/SRV 记录与 additional
#      —— 不依赖组播与 avahi,验收机(WSL2 NAT)也能跑,见 mdns_query_test.py
#   2) 系统解析级(可选):有 avahi-resolve 就顺手看一眼真实解析结果;
#      没有则打印提示(组播不可达 ≠ 应答器有问题,故不计入成败)
#
# 用法:./mdns_test.sh [设备IP|主机名] [期望主机名] [期望端口]
#   不传地址时默认 127.0.0.1(本地模拟器)。
set -uo pipefail
cd "$(dirname "$0")"                  # tests/web

target="${1:-127.0.0.1}"
host="${2:-doorguard}"
port="${3:-8080}"
FAIL=0

echo "== 1. 报文级检查($target:5353)=="
if python3 mdns_query_test.py "$target" 5353 "$host" "$port"; then
    echo "报文级检查:通过"
else
    echo "报文级检查:失败"
    FAIL=1
fi

echo "== 2. 系统解析(可选)=="
if command -v avahi-resolve > /dev/null 2>&1; then
    if ip=$(avahi-resolve -n "$host.local" 2>/dev/null | awk '{print $2}'); then
        [ -n "$ip" ] && echo "avahi 解析 $host.local → $ip" || echo "avahi 未解析到(组播可能不通)"
    fi
else
    echo "未安装 avahi-resolve,跳过(板/同网段 Linux 可用 avahi-browse -at 验证发现)"
fi
echo "兜底:WSL /etc/hosts 可加 '<板IP> $host.local'"

echo
if [ "$FAIL" = 0 ]; then
    echo "mDNS 验收通过"
else
    echo "mDNS 验收失败"
fi
exit "$FAIL"
