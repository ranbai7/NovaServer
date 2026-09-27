#!/usr/bin/env python3
"""验证 Acceptor::handle_fd_exhausted 的兜底行为。

用法: emfile_test.py <port> <连接数>

先用大量连接占住服务端的描述符，再在"已耗尽"的状态下发起一次探测请求：
- 描述符充足时，探测会拿到正常的 200 响应
- 描述符耗尽时，新连接会走到 handle_fd_exhausted 的「建立即关闭」路径，
  客户端表现为连接被立即关闭 / RST / 无响应

两者的差异即证明 EMFILE 分支确实被走到，且服务端没有因此崩溃。
"""
import socket
import struct
import sys
import time

port = int(sys.argv[1])
count = int(sys.argv[2])

linger = struct.pack("ii", 1, 0)  # 关闭时发 RST，服务端立即感知


def probe(tag):
    """发一次请求，返回观察到的结果"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
    except OSError as exc:
        print(f"  [{tag}] 连不上: {exc}")
        return
    try:
        s.sendall(b"GET /judge.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        s.settimeout(2)
        data = s.recv(200)
        if data:
            print(f"  [{tag}] 收到 {len(data)} 字节: {data.splitlines()[0].decode(errors='replace')}")
        else:
            print(f"  [{tag}] 连接被立即关闭（EMFILE 兜底路径）")
    except socket.timeout:
        print(f"  [{tag}] 超时无响应")
    except ConnectionResetError:
        print(f"  [{tag}] 被 RST（EMFILE 兜底路径）")
    finally:
        s.close()


# —— 阶段一：描述符充足时探测，作为对照 ——
probe("充足")

# —— 阶段二：占满描述符后再探测 ——
socks = []
for i in range(count):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        socks.append(s)
    except OSError as exc:
        print(f"  客户端第 {i} 条失败: {exc}")
        break
print(f"  已占用 {len(socks)} 条连接")
time.sleep(1)

probe("耗尽")

for s in socks:
    s.close()
