#!/usr/bin/env python3
"""classifieds chat realtime e2e (A1.6 part 2) over the binary WS protocol.

Proves the live path: a conversation PARTICIPANT subscribes to its messages (VIA
membership), the counterpart sends a message over HTTP, and the engine pushes a
CHANGE frame to the subscriber — and a NON-participant's subscribe is denied.

Self-contained: builds the bundle db (schema + seed), boots cellar with the
bundle policy + seeded users, then mixes an HTTP client and a raw WS client.
Usage: test_chat_ws.py <cellar-binary>
"""
import base64, hashlib, json, os, socket, struct, sys, time, sqlite3, subprocess, tempfile, shutil
import http.client

HERE = os.path.dirname(os.path.abspath(__file__))
ADMIN  = ("admin@cls.dev", "adminpw01")
BUYER  = ("buyer@cls.dev", "buyerpw01")
SELLER = ("seller@cls.dev", "sellerpw01")
STRANGER = ("stranger@cls.dev", "strangerpw1")

HDR = struct.Struct("!BBHI")
OP_LOGIN, OP_SUBSCRIBE, OP_CHANGE = 0x10, 0x20, 0x22

ok = 0; fail = 0
def chk(name, cond, detail=""):
    global ok, fail
    print(f"  {'ok' if cond else 'FAIL':<5} {name:<40} {detail}")
    ok += bool(cond); fail += (not cond)

# ---- raw WS client (from tests/realtime_e2e.py) ----
def ws_connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    if b"101" not in buf.split(b"\r\n", 1)[0]:
        raise RuntimeError("ws handshake failed")
    return s

def ws_send(s, opcode, mid, obj):
    body = json.dumps(obj).encode()
    payload = HDR.pack(opcode, 0, mid, len(body)) + body
    n = len(payload)
    hdr = bytes([0x82])
    if n < 126:     hdr += bytes([0x80 | n])
    elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:           hdr += bytes([0x80 | 127]) + struct.pack("!Q", n)
    mask = os.urandom(4)
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

def _rd(s, n):
    d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c: raise ConnectionError("closed")
        d += c
    return d

def ws_recv(s, timeout=8):
    s.settimeout(timeout)
    while True:
        b0, b1 = _rd(s, 2)
        op = b0 & 0xF; ln = b1 & 0x7F
        if ln == 126:   ln = struct.unpack("!H", _rd(s, 2))[0]
        elif ln == 127: ln = struct.unpack("!Q", _rd(s, 8))[0]
        payload = _rd(s, ln) if ln else b""
        if op == 0x8: raise ConnectionError("server closed")
        if op in (0x1, 0x2):
            aop, _fl, _mid, plen = HDR.unpack(payload[:8])
            return aop, json.loads(payload[8:8 + plen] or b"{}")

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def wait_listen(port, proc, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None: return False
        with socket.socket() as s:
            s.settimeout(0.25)
            try: s.connect(("127.0.0.1", port)); return True
            except OSError: time.sleep(0.05)
    return False

def main():
    if len(sys.argv) < 2:
        print("usage: test_chat_ws.py <cellar-binary>", file=sys.stderr); return 2
    binary = sys.argv[1]
    d = tempfile.mkdtemp(prefix="cls-ws-")
    db = os.path.join(d, "data.db")
    try:
        con = sqlite3.connect(db)
        con.executescript(open(os.path.join(HERE, "schema.sql")).read())
        con.executescript(open(os.path.join(HERE, "seed.sql")).read())
        con.commit(); con.close()
        shutil.copy(os.path.join(HERE, "hooks.lua"), os.path.join(d, "hooks.lua"))
        shutil.copy(os.path.join(HERE, "policies.json"), os.path.join(d, "policies.json"))

        port = free_port()
        env = dict(os.environ, CEL_PORT=str(port), CEL_DATA_DB=db, CEL_LOG_LEVEL="warn",
                   CEL_POLICY_FILE=os.path.join(d, "policies.json"),
                   CEL_AUTH_RATELIMIT="0", CEL_API_RATELIMIT="0",
                   CEL_SEED_USERS=(f"{ADMIN[0]}:{ADMIN[1]}:admin;{BUYER[0]}:{BUYER[1]}:user;"
                                   f"{SELLER[0]}:{SELLER[1]}:user;{STRANGER[0]}:{STRANGER[1]}:user"))
        log = open(os.path.join(d, "server.log"), "w")
        proc = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if not wait_listen(port, proc):
                print(open(os.path.join(d, "server.log")).read(), file=sys.stderr); return 2
            run(port)
        finally:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
            log.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print(f"\n{'ALL PASS' if fail == 0 else f'FAILED ({fail})'}  ({ok} ok)")
    return 1 if fail else 0

def run(port):
    HOST = "127.0.0.1"
    def hreq(method, path, body=None, token=None):
        c = http.client.HTTPConnection(HOST, port, timeout=10)
        h = {"Content-Type": "application/json"} if body is not None else {}
        if token: h["Authorization"] = "Bearer " + token
        c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
        r = c.getresponse(); data = r.read(); c.close()
        try: return r.status, json.loads(data)
        except Exception: return r.status, None
    def tok(creds):
        return (hreq("POST", "/auth/login", {"email": creds[0], "password": creds[1]})[1] or {}).get("token")

    print(f"== classifieds chat WS e2e -> {HOST}:{port} ==")
    seller, buyer_t, stranger = tok(SELLER), tok(BUYER), tok(STRANGER)
    chk("logins", all([seller, buyer_t, stranger]))

    # seller posts a chat listing; buyer opens a conversation
    s, b = hreq("POST", "/api/listings",
                {"category_id": "cat-cars", "title": "WS Chat Car", "price": 9000, "city_id": "ci-bishkek",
                 "allow_chat": 1, "attributes": {"make": "Toyota", "year": 2018}}, token=seller)
    lid = (b or {}).get("row", {}).get("id"); chk("listing -> 201", s == 201, f"status={s}")
    s, b = hreq("POST", "/rpc/start_conversation", {"listing_id": lid}, token=buyer_t)
    conv = ((b or {}).get("result") or {}).get("conversation_id"); chk("conversation", bool(conv))

    # buyer subscribes (VIA membership) to this conversation's messages
    w = ws_connect(HOST, port)
    ws_send(w, OP_LOGIN, 1, {"email": BUYER[0], "password": BUYER[1]})
    _op, body = ws_recv(w); btok = body.get("token"); chk("ws login buyer", bool(btok))
    ws_send(w, OP_SUBSCRIBE, 2, {"token": btok, "table": "message",
                                 "key": {"column": "conversation_id", "value": conv}})
    _op, body = ws_recv(w)
    chk("participant subscribe ok", body.get("status") == "ok", str(body))

    # seller sends a message over HTTP -> buyer must receive a live CHANGE
    msg = "live ping " + base64.b16encode(os.urandom(3)).decode()
    s, _ = hreq("POST", "/api/message", {"conversation_id": conv, "body": msg}, token=seller)
    chk("seller sends -> 201", s == 201, f"status={s}")
    change = None
    for _ in range(6):
        try: aop, body = ws_recv(w)
        except (socket.timeout, ConnectionError): break
        if aop == OP_CHANGE: change = body; break
    chk("buyer receives live CHANGE", change is not None, str(change))
    if change:
        chk("change.table message", change.get("table") == "message", str(change.get("table")))
        chk("change.row body matches", (change.get("row") or {}).get("body") == msg, msg)
    w.close()

    # a non-participant cannot subscribe to the conversation (VIA membership denies)
    w2 = ws_connect(HOST, port)
    ws_send(w2, OP_LOGIN, 1, {"email": STRANGER[0], "password": STRANGER[1]})
    _op, body = ws_recv(w2); stok = body.get("token")
    ws_send(w2, OP_SUBSCRIBE, 2, {"token": stok, "table": "message",
                                  "key": {"column": "conversation_id", "value": conv}})
    _op, body = ws_recv(w2)
    chk("stranger subscribe denied", body.get("status") != "ok", str(body))
    w2.close()

if __name__ == "__main__":
    sys.exit(main())
