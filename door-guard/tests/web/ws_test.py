#!/usr/bin/env python3
"""WebSocket 实时推送验收(独立可跑):鉴权 + 服务端主动推送。

为什么不用第三方库:板/宿主都只有 python3 标准库,裸 socket 更能确认
"服务端一开始就 401/101"这种协议级行为。

覆盖:
  1. 不带 token 握手 → 401(未授权连接必须被拒,不能裸奔推送门禁事件)
  2. 带 token 握手 → 101
  3. **客户端全程不发任何消息**,仅由服务端推送:触发一次 NTP →
     应在 30s 内收到 {"type":"ntp",...}(旧实现要客户端每 2s 发 ping
     才排水,这一条会失败——正是本次改造要锁住的行为)
用法:python3 ws_test.py [host] [port] [user] [pwd]
"""
import base64, hashlib, json, os, socket, struct, sys, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
USER = sys.argv[3] if len(sys.argv) > 3 else "admin"
PWD = sys.argv[4] if len(sys.argv) > 4 else "admin"

MAGIC = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def handshake(path, timeout=5):
    s = socket.create_connection((HOST, PORT), timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET {path} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    return s, head.decode(errors="replace"), rest


def read_frame(s, rest, timeout=5):
    """读一个 WS 帧;返回 (opcode, payload, 剩余缓冲)"""
    s.settimeout(timeout)
    buf = rest
    while len(buf) < 2:
        chunk = s.recv(4096)
        if not chunk:
            return None, b"", b""
        buf += chunk
    opcode = buf[0] & 0x0F
    ln = buf[1] & 0x7F
    need = 2
    if ln == 126:
        while len(buf) < 4:
            buf += s.recv(4096)
        ln = struct.unpack(">H", buf[2:4])[0]
        need = 4
    elif ln == 127:
        while len(buf) < 10:
            buf += s.recv(4096)
        ln = struct.unpack(">Q", buf[2:10])[0]
        need = 10
    while len(buf) < need + ln:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    return opcode, buf[need:need + ln], buf[need + ln:]


def api(path, token=None, method="GET", body=None):
    req = urllib.request.Request(f"http://{HOST}:{PORT}{path}", method=method)
    if token:
        req.add_header("X-Auth-Token", token)
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data, timeout=10) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except Exception:
            return e.code, {}


def main():
    fails = []

    # 1 未授权
    s, head, _ = handshake("/api/ws")
    s.close()
    line = head.split("\r\n")[0]
    if " 401 " in line:
        print("PASS: 未带 token 的 WebSocket 被拒(401)")
    else:
        fails.append(f"未带 token 应 401,实际: {line}")

    st, j = api("/api/login", method="POST", body={"user": USER, "pwd": PWD})
    token = j.get("token", "")
    if st != 200 or not token:
        print(f"FAIL: 登录失败({st}),无法继续")
        return 1
    print("PASS: 登录拿到 token")

    # 2 授权握手
    s, head, rest = handshake("/api/ws?token=" + token)
    line = head.split("\r\n")[0]
    if " 101 " in line:
        print("PASS: 授权 WebSocket 握手 101")
    else:
        print(f"FAIL: 授权握手失败: {line}")
        return 1

    # 3 服务端主动推送(客户端不发消息)
    st, _ = api("/api/ntp", token=token, method="POST")
    if st not in (200, 202, 409):
        print(f"FAIL: NTP 触发返回 {st}")
        return 1
    print(f"PASS: NTP 触发已受理({st}),等待服务端推送…")

    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            opcode, payload, rest = read_frame(s, rest, timeout=5)
        except socket.timeout:
            continue
        if opcode is None:
            break
        if opcode in (1, 2) and payload:
            try:
                j = json.loads(payload.decode(errors="replace"))
            except Exception:
                continue
            print("PASS: 收到服务端主动推送:", json.dumps(j, ensure_ascii=False)[:140])
            if j.get("type") == "ntp":
                s.close()
                return 1 if fails else 0
    fails.append("30s 内未收到服务端主动推送(NTP 结果)")
    s.close()
    for f in fails:
        print("FAIL:", f)
    return 1


if __name__ == "__main__":
    sys.exit(main())
