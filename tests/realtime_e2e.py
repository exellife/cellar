#!/usr/bin/env python3
"""pgforge realtime end-to-end (#53), over the binary WS protocol.

Proves the live wiring the DB-free realtime_test can't: a client SUBSCRIBEs to a
realtime-enabled table, a write goes through the engine, and the engine pushes a
CHANGE frame to the subscriber. Self-contained raw WS client (handshake + masked
framing). Run as: realtime_e2e.py ws://127.0.0.1:<port>/
"""
import base64, hashlib, json, os, socket, struct, sys
from urllib.parse import urlparse

HDR = struct.Struct("!BBHI")              # opcode, flags, message_id, payload_len
OP_LOGIN, OP_SUBSCRIBE, OP_CHANGE, OP_DB_CREATE = 0x10, 0x20, 0x22, 0xD2
# A regular (owner-scoped) user: its writes force owner_id to itself and its
# subscription is scoped to owner_id = itself — so a delivered CHANGE proves the
# authz filter, not just a catch-all.
USER = ("editor@pgforge.dev", "editor-pw")


def connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    if b"101" not in buf.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"handshake failed: {buf[:80]!r}")
    return s


def ws_send(s, opcode, mid, obj):
    body = json.dumps(obj).encode()
    payload = HDR.pack(opcode, 0, mid, len(body)) + body
    n = len(payload)
    hdr = bytes([0x82])                   # FIN + binary
    if n < 126:      hdr += bytes([0x80 | n])
    elif n < 65536:  hdr += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:            hdr += bytes([0x80 | 127]) + struct.pack("!Q", n)
    mask = os.urandom(4)
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def _rd(s, n):
    d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c:
            raise ConnectionError("connection closed")
        d += c
    return d


def ws_recv_app(s, timeout=8):
    """Return (app_opcode, dict) for the next binary frame (skips ping/pong)."""
    s.settimeout(timeout)
    while True:
        b0, b1 = _rd(s, 2)
        op = b0 & 0xF
        ln = b1 & 0x7F
        if ln == 126:   ln = struct.unpack("!H", _rd(s, 2))[0]
        elif ln == 127: ln = struct.unpack("!Q", _rd(s, 8))[0]
        payload = _rd(s, ln) if ln else b""
        if op == 0x8:                      # close
            raise ConnectionError("server closed")
        if op in (0x1, 0x2):               # text/binary app frame
            aop, _fl, _mid, plen = HDR.unpack(payload[:8])
            return aop, json.loads(payload[8:8 + plen] or b"{}")


def main():
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    host, port = u.hostname or "127.0.0.1", u.port or 8080
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<30} {detail}")
        ok += bool(cond); fail += (not cond)

    s = connect(host, port)

    ws_send(s, OP_LOGIN, 1, {"email": USER[0], "password": USER[1]})
    _op, body = ws_recv_app(s)
    token = body.get("token")
    chk("login", bool(token))

    ws_send(s, OP_SUBSCRIBE, 2, {"token": token, "table": "notes"})
    _op, body = ws_recv_app(s)
    chk("subscribe ok", body.get("status") == "ok", str(body))

    title = "rt-" + base64.b16encode(os.urandom(4)).decode()
    ws_send(s, OP_DB_CREATE, 3, {"token": token, "table": "notes", "values": {"title": title}})

    # The create response and the pushed CHANGE both arrive; find the CHANGE (0x22).
    change = None
    for _ in range(6):
        try:
            aop, body = ws_recv_app(s)
        except (socket.timeout, ConnectionError):
            break
        if aop == OP_CHANGE:
            change = body; break
    chk("CHANGE pushed", change is not None, str(change))
    if change:
        chk("change.table", change.get("table") == "notes", str(change.get("table")))
        chk("change.op", change.get("op") == "INSERT", str(change.get("op")))
        chk("change.row", (change.get("row") or {}).get("title") == title, title)

    s.close()
    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
