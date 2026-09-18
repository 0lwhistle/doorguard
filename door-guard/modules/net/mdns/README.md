# mDNS 应答器(doorguard.local + _http._tcp 服务发现)

目的:局域网里**不用查 IP**——浏览器直接开 `http://doorguard.local:8080`,
手机/电脑的服务发现也能看到这台设备。

为什么自实现:avahi 不在 B4 rootfs(spec-network §1),而门禁需要的只是
"能按名字找到设备";完整服务发现栈(缓存/代理/冲突重命名循环)属过度设计。

## 文件构成

| 文件 | 职责 |
|---|---|
| `mdns_wire.c/h` | 报文编解码(纯函数:名字/压缩指针/记录写入/解析),可单测 |
| `mdns_responder.c/h` | socket + 状态机(探测/通告/常驻应答)+ 与配置对接 |

分层的意义:报文是字节级协议,写错只能靠抓包;拆出纯函数后可以对着
RFC 1035 §4.1 的字节布局写断言(`tests/test_mdns_wire.c`,69 项)。

## 行为(RFC 6762 的必要子集)

状态机:`PROBING`(3 次探测,250ms 间隔)→ `ANNOUNCING`(2 次推送式通告)
→ `READY`(常驻应答)。

公告的记录:

| 名字 | 类型 | 说明 |
|---|---|---|
| `<host>.local` | A | 本机 IPv4(TTL 120s) |
| `_http._tcp.local` | PTR | 服务发现入口(手机 / `avahi-browse -at` 靠它) |
| `<host>._http._tcp.local` | SRV | 端口取 `web_port`(默认 8080) |
| `<host>._http._tcp.local` | TXT | `txtvers=1`、`path=/` |

应答策略:
- 查询来自 5353 且未置 QU 位 → **组播应答**(带 cache-flush 位);
- 来自其它端口(legacy resolver)或置了 QU 位 → **单播应答**:回带查询 ID 与问题段,
  TTL 压到 10s(不污染组播缓存),不带 cache-flush;
- SRV 的目标 A 记录放 **additional 段**,客户端一次拿全,少一轮往返;
- 只答自己的名字,其它查询一律不理(避免"见谁答谁"污染局域网)。

其他保证:
- **IP 变化重新通告**(拔插网线 / DHCP 换租约):每 5s 查一次地址,变了立刻重发通告
  ——"按名字访问突然失灵"最常见的原因就是缓存里还是旧地址;
- **重名处理**:探测期发现同名不同 IP → 改名 `doorguard-2`(后缀递增,最多 -9)并写回
  `device_config.mdns_host`,重启后名字稳定;
- **关机 goodbye**(TTL=0):停止服务时发一轮,不留幽灵条目;
- 逐接口入组(`IP_ADD_MEMBERSHIP` per iface)+ `IP_PKTINFO` 取来源网口,
  双网口(eth0+eth1)时从正确的网口应答;
- 与 avahi / 手机热点共存:`SO_REUSEPORT` 抢不到 5353 也不影响别家。

主机名配置:`device_config.mdns_host`(默认 `doorguard`,最长 39 字符)。
改端口/名字后调用 `mdns_announce()` 立即刷新局域网缓存。

## 单测覆盖

`tests/test_mdns_wire.c`:名字编码边界(空标签/超长标签/根)、压缩指针与防环/越界、
名字大小写比较、问题段解析(QU 位/多问题/坏报文)、记录写入逐字节对拍、
TTL=0 goodbye 形态、小缓冲不越界。

## 测试

```bash
# 报文级(必跑,不依赖组播/avahi,WSL2 也可):QU 单播查询 → 验 A/PTR/SRV/additional
python3 tests/web/mdns_query_test.py 127.0.0.1 5353 doorguard 8080
./tests/web/mdns_test.sh          # 同上 + 有 avahi 时顺带看一眼真实解析结果

ctest --test-dir build-tests -R test_mdns_wire   # 纯逻辑单测
```

## 环境说明(WSL2)

WSL2 是 NAT 网络,**组播从 WSL 到 LAN 常不可达**,`avahi-resolve` 可能拿不到结果
——这不能说明应答器有问题(报文级测试才是判据)。验收建议:
- 板上或同网段 Linux:`avahi-browse -at | grep -i doorguard` 应能看到 `_http._tcp` 服务;
- Windows 10+ 原生支持 mDNS,直接在浏览器开 `http://doorguard.local:8080`;
- WSL 兜底:在 `/etc/hosts` 加 `<板IP> doorguard.local`。
