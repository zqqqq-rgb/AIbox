#!/usr/bin/env python3
"""M4 冒烟 WS 探针（纯标准库）：连接 /ws/meta/{cam}，统计 N 秒内收到的 detections 帧数。

用法：
  python3 scripts/ws_probe.py --port 18081 --path /ws/meta/1 --seconds 6 --min 3

输出：detections=N other=M；exit 0 当 detections >= --min（业务失败不退出——
中途断连等异常按已收帧数判定，供冒烟脚本汇总）。
"""
import argparse
import base64
import os
import socket
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--path", default="/ws/meta/1")
ap.add_argument("--seconds", type=float, default=6.0)
ap.add_argument("--min", type=int, default=3, dest="min_frames")
a = ap.parse_args()

key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET {a.path} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")

try:
    s = socket.create_connection(("127.0.0.1", a.port), timeout=3)
except OSError as e:
    print(f"connect fail: {e}")
    sys.exit(1)
s.sendall(req.encode())

buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        print("handshake closed by server")
        sys.exit(1)
    buf += chunk
head, _, rest = buf.partition(b"\r\n\r\n")
if b"101" not in head.split(b"\r\n")[0]:
    print("handshake rejected:", head[:80].decode(errors="replace"))
    sys.exit(1)

det = other = 0
payload = rest
deadline = time.time() + a.seconds


def need(n):
    global payload
    while len(payload) < n:
        try:
            s.settimeout(max(0.1, deadline - time.time()))
            chunk = s.recv(4096)
        except (socket.timeout, OSError):
            return False
        if not chunk:
            return False
        payload += chunk
    return True


while time.time() < deadline:
    if not need(2):
        break
    op = payload[0] & 0x0F
    ln = payload[1] & 0x7F
    off = 2
    if ln == 126:
        if not need(4):
            break
        ln = int.from_bytes(payload[2:4], "big")
        off = 4
    elif ln == 127:
        if not need(10):
            break
        ln = int.from_bytes(payload[2:10], "big")
        off = 10
    if not need(off + ln):
        break
    body = payload[off:off + ln]
    payload = payload[off + ln:]
    if op == 0x1:  # text
        txt = body.decode(errors="replace")
        if "detections" in txt:
            det += 1
        else:
            other += 1
    elif op == 0x8:  # close
        break
    # ping(9)/pong(10)/binary(2)：忽略

print(f"detections={det} other={other}")
sys.exit(0 if det >= a.min_frames else 1)
