#!/usr/bin/env python3
"""WS keepalive (idle ping/pong reaping). With CEL_WS_PING_INTERVAL/PONG_TIMEOUT set,
portico must PING an idle WebSocket and reap a peer that never PONGs, while a peer that
does PONG survives past the reap window. Booted with CEL_WS_PING_INTERVAL=1
CEL_WS_PONG_TIMEOUT=2 (ws_keepalive_test.sh). Self-contained raw WS client.
"""
import base64, os, socket, struct, sys, time
from urllib.parse import urlparse


def connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    k = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {k}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    b = b""
    while b"\r\n\r\n" not in b:
        b += s.recv(4096)
    if b"101" not in b.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"handshake failed: {b[:80]!r}")
    return s


def read_frame(s):
    b0 = s.recv(1)
    if not b0:
        return None, None
    op = b0[0] & 0x0F
    ln = s.recv(1)[0] & 0x7F
    if ln == 126:
        ln = struct.unpack("!H", s.recv(2))[0]
    elif ln == 127:
        ln = struct.unpack("!Q", s.recv(8))[0]
    p = b""
    while len(p) < ln:
        p += s.recv(ln - len(p))
    return op, p


def pong(s, payload=b""):
    n = len(payload)
    m = os.urandom(4)
    s.sendall(bytes([0x8A, 0x80 | n]) + m + bytes(c ^ m[i % 4] for i, c in enumerate(payload)))


def main():
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    host, port = u.hostname or "127.0.0.1", u.port or 8080
    ok = fail = 0

    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<42} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== WS keepalive -> {host}:{port} (ping_interval=1, pong_timeout=2) ==")

    # Phase A: never PONG -> server PINGs the idle socket then reaps it.
    s = connect(host, port); s.settimeout(8)
    got_ping = reaped = False; t = time.time()
    while time.time() - t < 8:
        try:
            op, _ = read_frame(s)
            if op is None:
                reaped = True; break
            if op == 0x9:
                got_ping = True
            if op == 0x8:
                reaped = True; break
        except socket.timeout:
            break
    s.close()
    chk("idle connection gets a server PING", got_ping)
    chk("peer that never PONGs is reaped", reaped)

    # Phase B: PONG back -> survive past the reap window, keep getting PINGs.
    s = connect(host, port); s.settimeout(8)
    pings = 0; alive = True; t = time.time()
    while time.time() - t < 5:
        try:
            op, p = read_frame(s)
            if op is None:
                alive = False; break
            if op == 0x9:
                pings += 1; pong(s, p)
            if op == 0x8:
                alive = False; break
        except socket.timeout:
            pass
    s.close()
    chk("PONGing peer keeps getting PINGs", pings > 0, f"pings={pings}")
    chk("PONGing peer survives the reap window", alive)

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
