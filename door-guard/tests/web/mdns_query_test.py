#!/usr/bin/env python3
"""mDNS 应答器验收(裸 socket,不问 avahi):确认设备"局域网按名字能找到"。

为什么不用 avahi-resolve:验收机(WSL2)是 NAT 网络,组播常不可达,而且
avahi 不一定装。直接向应答器发 **QU(请求单播应答)** 查询走 127.0.0.1,
按 RFC 6762 §6.7 走 legacy 单播路径,既能验报文正确性,又不依赖组播环境。

覆盖:
  1. A 查询 doorguard.local → 单播应答,事务 ID 回带,ANCOUNT≥1,A 记录 4 字节
  2. PTR 查询 _http._tcp.local → 服务发现记录指向 <host>._http._tcp.local
  3. SRV 查询 <host>._http._tcp.local → 端口=web_port,目标=<host>.local,
     additional 段带目标 A 记录(客户端不必再问一轮)
  4. 名字不匹配的查询 → 不应答(不能见谁答谁)
用法:python3 mdns_query_test.py [host] [port] [期望主机名] [期望端口]
"""
import socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5353
EXPECT_HOST = sys.argv[3] if len(sys.argv) > 3 else "doorguard"
EXPECT_PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 8080

TYPE_A, TYPE_PTR, TYPE_SRV, TYPE_TXT, TYPE_ANY = 1, 12, 33, 16, 255
CLASS_IN, CLASS_TOP = 1, 0x8000


def enc_name(name):
    out = b""
    for lab in name.split("."):
        out += bytes([len(lab)]) + lab.encode()
    return out + b"\x00"


def dec_name(pkt, off):
    out, jumped, hops = [], False, 0
    pos = off
    while True:
        ln = pkt[pos]
        if ln == 0:
            pos += 1
            break
        if ln & 0xC0 == 0xC0:
            if not jumped:
                consumed = pos - off + 2
            jumped = True
            pos = ((ln & 0x3F) << 8) | pkt[pos + 1]
            hops += 1
            if hops > 16:
                raise ValueError("压缩指针成环")
            continue
        out.append(pkt[pos + 1:pos + 1 + ln].decode(errors="replace"))
        pos += 1 + ln
    return ".".join(out), (consumed if jumped else pos - off)


def query(name, qtype, timeout=3.0):
    """发 QU 查询(单播应答),返回 (rcode, answers, additionals)"""
    qid = 0x4321
    pkt = struct.pack(">HHHHHH", qid, 0, 1, 0, 0, 0)
    pkt += enc_name(name) + struct.pack(">HH", qtype, CLASS_IN | CLASS_TOP)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(pkt, (HOST, PORT))
    try:
        data, _ = s.recvfrom(4096)
    except socket.timeout:
        return None
    finally:
        s.close()

    rid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
    off = 12
    for _ in range(qd):
        _, used = dec_name(data, off)
        off += used + 4
    records = []
    for _ in range(an + ns + ar):
        nm, used = dec_name(data, off)
        off += used
        rtype, rclass, ttl, rdlen = struct.unpack(">HHIH", data[off:off + 10])
        off += 10
        rdata = data[off:off + rdlen]
        records.append((nm, rtype, rclass, ttl, rdata))
        off += rdlen
    return (rid, flags, records, an, ar)


def main():
    fails = []

    # 1 A 记录
    r = query(f"{EXPECT_HOST}.local", TYPE_A)
    if r is None:
        print("FAIL: A 查询无应答")
        return 1
    rid, flags, recs, an, ar = r
    a = [x for x in recs if x[1] == TYPE_A]
    if rid != 0x4321:
        fails.append(f"未回带事务 ID(得到 {rid:#x})")
    if not (flags & 0x8000):
        fails.append("应答未置 QR 位")
    if not a:
        fails.append("应答里没有 A 记录")
    elif len(a[0][4]) != 4:
        fails.append(f"A 记录长度异常: {len(a[0][4])}")
    else:
        ip = ".".join(str(b) for b in a[0][4])
        print(f"PASS: {EXPECT_HOST}.local → A {ip}(TTL {a[0][3]}s)")
    if a and a[0][3] > 120:
        fails.append(f"单播应答 TTL 应压缩到 ≤120s,得到 {a[0][3]}")

    # 2 服务发现 PTR
    r = query("_http._tcp.local", TYPE_PTR)
    if r is None:
        print("FAIL: PTR 查询无应答")
        return 1
    rid, flags, recs, an, ar = r
    ptrs = [x for x in recs if x[1] == TYPE_PTR]
    if not ptrs:
        fails.append("应答里没有 PTR 记录")
    else:
        target, _ = dec_name(ptrs[0][4], 0)   # rdata 内是完整名字(本实现不压缩)
        want = f"{EXPECT_HOST}._http._tcp.local"
        if target.rstrip(".") != want:
            fails.append(f"PTR 指向 {target},期望 {want}")
        else:
            print(f"PASS: _http._tcp.local → PTR {target}")

    # 3 SRV + additional A
    inst = f"{EXPECT_HOST}._http._tcp.local"
    r = query(inst, TYPE_SRV)
    if r is None:
        print("FAIL: SRV 查询无应答")
        return 1
    rid, flags, recs, an, ar = r
    srvs = [x for x in recs if x[1] == TYPE_SRV]
    if not srvs:
        fails.append("应答里没有 SRV 记录")
    else:
        rd = srvs[0][4]
        prio, weight, port = struct.unpack(">HHH", rd[:6])
        target, _ = dec_name(rd[6:], 0)
        if port != EXPECT_PORT:
            fails.append(f"SRV 端口 {port},期望 {EXPECT_PORT}")
        elif target.rstrip(".") != f"{EXPECT_HOST}.local":
            fails.append(f"SRV 目标 {target},期望 {EXPECT_HOST}.local")
        else:
            print(f"PASS: SRV {inst} → {target}:{port}")
        extra = [x for x in recs if x[1] == TYPE_A]
        if not extra:
            fails.append("SRV 应答缺 additional A 记录(客户端要多问一轮)")
        else:
            print("PASS: SRV 应答附带目标 A 记录(additional 段)")

    # 4 不匹配的名字不应答
    if query("not-doorguard.local", TYPE_A, timeout=2.0) is not None:
        fails.append("对不匹配的名字也应答了(应答面过宽)")
    else:
        print("PASS: 不匹配名字不应答")

    for f in fails:
        print("FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
