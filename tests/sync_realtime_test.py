#!/usr/bin/env python3
"""Regression: a soft-delete on a SYNCABLE table still pushes a realtime DELETE.

The tombstone read-filter (deleted=0) must NOT gate realtime *delivery* — a soft-
delete emits a DELETE event whose row carries deleted=1, and the owner must still
receive it (to drop the row live). This guards the fix where the subscription
delivery predicates skip the syncable `deleted=0` rule. Subscribes to the syncable
`items` table (realtime-enabled in config/policies.json), then asserts INSERT and
DELETE events both arrive. Self-contained raw WS client (handshake + masked framing).
"""
import base64, json, os, socket, struct, sys, time
import urllib.request as U
from urllib.parse import urlparse

HDR = struct.Struct("!BBHI")                       # opcode, flags, message_id, len
OP_SUBSCRIBE, OP_CHANGE = 0x20, 0x22
ADMIN = ("admin@cellar.dev", "s3cret-admin")


def login(host, port):
    r = U.Request(f"http://{host}:{port}/auth/login",
                  data=json.dumps({"email": ADMIN[0], "password": ADMIN[1]}).encode(),
                  headers={"Content-Type": "application/json"})
    return json.load(U.urlopen(r))["token"]


def api(host, port, method, path, tok, body=None):
    r = U.Request(f"http://{host}:{port}{path}",
                  data=(json.dumps(body).encode() if body is not None else None),
                  headers={"Content-Type": "application/json", "Authorization": "Bearer " + tok},
                  method=method)
    return json.load(U.urlopen(r))


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


def send(s, op, mid, obj):
    body = json.dumps(obj).encode()
    pl = HDR.pack(op, 0, mid, len(body)) + body
    n = len(pl)
    h = bytes([0x82]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack("!H", n))
    m = os.urandom(4)
    s.sendall(h + m + bytes(c ^ m[i % 4] for i, c in enumerate(pl)))


def recv(s, timeout):
    s.settimeout(timeout)
    b0 = s.recv(1); b1 = s.recv(1)
    ln = b1[0] & 0x7F
    if ln == 126: ln = struct.unpack("!H", s.recv(2))[0]
    d = b""
    while len(d) < ln: d += s.recv(ln - len(d))
    aop, _, _, plen = HDR.unpack(d[:8])
    return aop, json.loads(d[8:8 + plen] or b"{}")


def wait_change(s, op, want_id, timeout=2.5):
    end = time.time() + timeout
    while time.time() < end:
        try: aop, b = recv(s, max(0.1, end - time.time()))
        except (socket.timeout, OSError): break
        if aop == OP_CHANGE and b.get("op") == op and (b.get("row") or {}).get("id") == want_id:
            return b
    return None


def main():
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    host, port = u.hostname or "127.0.0.1", u.port or 8080
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<46} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar sync-realtime harness -> {host}:{port} ==")
    tok = login(host, port)
    s = connect(host, port)
    send(s, OP_SUBSCRIBE, 1, {"token": tok, "table": "items"})
    _op, b = recv(s, 3)
    chk("subscribe items -> ok", b.get("status") == "ok", str(b))

    tid = api(host, port, "POST", "/api/items", tok, {"name": "rt"})["row"]["id"]
    chk("create -> INSERT event delivered", wait_change(s, "INSERT", tid) is not None)

    api(host, port, "DELETE", f"/api/items/{tid}", tok)
    ev = wait_change(s, "DELETE", tid)
    chk("soft-delete -> DELETE event delivered", ev is not None)
    chk("DELETE event carries the tombstone (deleted=1)",
        ev is not None and (ev.get("row") or {}).get("deleted") == 1, str(ev))

    # a write that arrives via /sync/push must ALSO reach live subscribers (push and
    # the realtime feed are the same change stream).
    pid = "push-rt-1"
    api(host, port, "POST", "/sync/push", tok,
        {"mutations": [{"op": "put", "table": "items", "id": pid, "values": {"name": "viaPush"}}]})
    chk("push create -> INSERT event delivered", wait_change(s, "INSERT", pid) is not None)
    api(host, port, "POST", "/sync/push", tok,
        {"mutations": [{"op": "del", "table": "items", "id": pid, "base_rev": 1}]})
    chk("push delete -> DELETE event delivered", wait_change(s, "DELETE", pid) is not None)

    s.close()
    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
