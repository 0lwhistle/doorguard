#!/usr/bin/env python3
"""WebSocket 实时事件验收:连接 /api/ws,触发 mock 认证事件,断言收到推送。"""
import base64, hashlib, json, os, socket, struct, sys, time

HOST = "127.0.0.1"
PORT = 8080

def ws_connect(path="/api/ws"):
    s = socket.create_connection((HOST, PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET {path} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(4096)
    if b"101" not in resp.split(b"\r\n")[0]:
        raise RuntimeError("握手失败: " + resp[:100].decode(errors="replace"))
    accept = base64.b64encode(hashlib.sha1(
        (key + "258EAFA5-E914-47DA-95CA-5CAB0DC85B11").encode()).digest())
    magic = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    expect = base64.b64encode(hashlib.sha1((key + magic.decode()).encode())).decode()
    if expect.encode() not in resp:
        raise RuntimeError("accept 校验失败")
    return s

def ws_read(s, timeout=8):
    s.settimeout(timeout)
    hdr = b""
    while len(hdr) < 2:
        hdr += s.recv(2 - len(hdr))
    opcode = hdr[0] & 0x0F
    ln = hdr[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", s.recv(2))[0]
    data = b""
    while len(data) < ln:
        data += s.recv(ln - len(data))
    return opcode, data.decode(errors="replace")

def main():
    s = ws_connect()
    print("WS 握手 OK", flush=True)
    # 推送为"客户端 ping 触发排水"模式:周期发 ping,读回排出的消息
    deadline = time.time() + 15
    last_ping = 0
    while time.time() < deadline:
        if time.time() - last_ping >= 2:
            s.sendall(b"\x81\x84\x00\x00\x00\x00ping")  # masked "ping"
            last_ping = time.time()
        try:
            op, text = ws_read(s, timeout=3)
        except socket.timeout:
            continue
        j = json.loads(text)
        print("收到推送:", json.dumps(j, ensure_ascii=False)[:120], flush=True)
        if j.get("type") == "auth":
            print("PASS: 收到实时认证事件", flush=True)
            return 0
    print("FAIL: 15s 内未收到 auth 推送(mock 未开?)", flush=True)
    return 1

if __name__ == "__main__":
    sys.exit(main())
