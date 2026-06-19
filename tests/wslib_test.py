#!/usr/bin/env python3
"""Adversarial WebSocket robustness harness for the vendored wslib.

Drives pgforge's OP_ECHO as a round-trip oracle using a hand-rolled raw-socket
WebSocket client, so we control every byte of every frame. Probes:

  - payload-size boundaries (WS length encoding 125/126/127, 16->64 bit, and
    wslib's 1K/4K/16K buffer-pool seams) up to the 1MB cap, and over it
  - fragmentation / continuation frames
  - control frames: ping->pong, unsolicited pong, close handshake
  - RFC framing rules: client masking required, reserved bits, bad opcodes
  - transport robustness: byte-at-a-time delivery, pipelined frames
  - concurrency: many parallel connections, rapid connect/disconnect churn

CRITICAL findings (wrong data, crash, hang) fail the run. RFC deviations are
reported as WARN — they're findings, not gating failures.
"""
import base64, hashlib, os, socket, struct, sys, threading, time
from urllib.parse import urlparse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# pgforge protocol
HDR = struct.Struct("!BBHI")
OP_ECHO, OP_PING_APP = 0x02, 0x01
FLAG_RESPONSE = 0x80

# WS opcodes
WS_CONT, WS_TEXT, WS_BIN, WS_CLOSE, WS_PING, WS_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


def app_frame(opcode, mid, payload=b""):
    return HDR.pack(opcode, 0, mid, len(payload)) + payload


class WS:
    """Minimal raw-socket WebSocket client with full frame control."""
    def __init__(self, host, port, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self._handshake(host, port)

    def _handshake(self, host, port):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\n"
               f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            d = self.sock.recv(4096)
            if not d:
                raise ConnectionError("handshake: connection closed")
            resp += d
        head, _, rest = resp.partition(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0]
        if b"101" not in status:
            raise ConnectionError(f"handshake failed: {status!r}")
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if accept.encode() not in head:
            raise ConnectionError("handshake: bad Sec-WebSocket-Accept")
        self.buf = rest

    def send_frame(self, opcode, payload=b"", fin=True, mask=True, rsv=0):
        b0 = (0x80 if fin else 0) | ((rsv & 0x7) << 4) | (opcode & 0xF)
        n = len(payload)
        out = bytes([b0])
        mbit = 0x80 if mask else 0x00
        if n < 126:
            out += bytes([mbit | n])
        elif n < 65536:
            out += bytes([mbit | 126]) + struct.pack("!H", n)
        else:
            out += bytes([mbit | 127]) + struct.pack("!Q", n)
        if mask:
            mk = os.urandom(4)
            out += mk
            payload = bytes(payload[i] ^ mk[i % 4] for i in range(n))
        out += payload
        self.sock.sendall(out)

    def _need(self, k):
        while len(self.buf) < k:
            d = self.sock.recv(1 << 20)
            if not d:
                raise ConnectionError("recv: connection closed")
            self.buf += d

    def recv_frame(self, timeout=10):
        self.sock.settimeout(timeout)
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        fin, rsv, opcode = b0 >> 7, (b0 >> 4) & 0x7, b0 & 0xF
        masked, ln = b1 >> 7, b1 & 0x7F
        off = 2
        if ln == 126:
            self._need(4); ln = struct.unpack("!H", self.buf[2:4])[0]; off = 4
        elif ln == 127:
            self._need(10); ln = struct.unpack("!Q", self.buf[2:10])[0]; off = 10
        if masked:           # servers must NOT mask
            off += 4
        self._need(off + ln)
        payload = self.buf[off:off + ln]
        self.buf = self.buf[off + ln:]
        return dict(fin=fin, rsv=rsv, opcode=opcode, masked=masked, payload=payload)

    def recv_app(self, timeout=10):
        """Return the next BINARY frame's payload, skipping pong/transparently."""
        while True:
            f = self.recv_frame(timeout)
            if f["opcode"] in (WS_BIN, WS_TEXT):
                return f
            if f["opcode"] == WS_CLOSE:
                raise ConnectionError("server sent CLOSE")
            # ignore PONG/PING and continue

    def echo(self, payload, mid=1):
        self.send_frame(WS_BIN, app_frame(OP_ECHO, mid, payload))
        f = self.recv_app()
        op, fl, rmid, plen = HDR.unpack(f["payload"][:8])
        return f, f["payload"][8:8 + plen]

    def close(self):
        try: self.sock.close()
        except OSError: pass


class Results:
    def __init__(self): self.crit = 0; self.warn = 0; self.ok = 0
    def ok_(self, name, detail=""):
        self.ok += 1; print(f"  ok    {name:<34} {detail}")
    def warn_(self, name, detail=""):
        self.warn += 1; print(f"  WARN  {name:<34} {detail}")
    def crit_(self, name, detail=""):
        self.crit += 1; print(f"  FAIL  {name:<34} {detail}")
    def check(self, name, cond, detail="", critical=True):
        (self.ok_ if cond else (self.crit_ if critical else self.warn_))(name, detail)
        return cond


def section(t): print(f"\n# {t}")


def test_sizes(host, port, R):
    section("payload sizes (round-trip echo)")
    sizes = [0, 1, 2, 125, 126, 127, 128, 1023, 1024, 1025,
             4095, 4096, 4097, 16383, 16384, 16385, 65535, 65536,
             100000, 500000, 1000000]
    for n in sizes:
        payload = os.urandom(n)
        try:
            ws = WS(host, port)
            f, got = ws.echo(payload)
            ws.close()
            if f["masked"]:
                R.check(f"size {n}: server frame masked", False, "server masked a frame (RFC violation)")
                continue
            R.check(f"size {n}", got == payload,
                    "" if got == payload else f"len {len(got)} != {n}")
        except Exception as e:
            R.crit_(f"size {n}", f"exception: {e!r}")


def test_over_limit(host, port, R):
    section("over-limit payload (>1MB cap)")
    big = os.urandom(2_000_000)
    crashed = False
    try:
        ws = WS(host, port)
        ws.send_frame(WS_BIN, app_frame(OP_ECHO, 1, big))
        try:
            f = ws.recv_frame(timeout=4)
            R.warn_("over-limit handled", f"server replied opcode={f['opcode']}")
        except (ConnectionError, socket.timeout):
            R.ok_("over-limit handled", "server closed/dropped (no echo) — acceptable")
        ws.close()
    except Exception as e:
        R.warn_("over-limit send", f"{e!r}")
    # server must still be alive
    try:
        ws = WS(host, port); _, got = ws.echo(b"alive?"); ws.close()
        crashed = got != b"alive?"
    except Exception as e:
        crashed = True; R.crit_("server alive after over-limit", f"{e!r}")
    if not crashed:
        R.ok_("server alive after over-limit")


def test_fragmentation(host, port, R):
    section("fragmentation (continuation frames)")
    payload = app_frame(OP_ECHO, 7, b"fragmented-hello-" + os.urandom(50))
    # split into 3 WS fragments: BIN/fin=0, CONT/fin=0, CONT/fin=1
    a, b, c = payload[:10], payload[10:25], payload[25:]
    try:
        ws = WS(host, port)
        ws.send_frame(WS_BIN, a, fin=False)
        ws.send_frame(WS_CONT, b, fin=False)
        ws.send_frame(WS_CONT, c, fin=True)
        f = ws.recv_app()
        op, fl, mid, plen = HDR.unpack(f["payload"][:8])
        got = f["payload"][8:8 + plen]
        ws.close()
        R.check("3-way fragmented echo", got == payload[8:], f"got {got[:20]!r}")
    except Exception as e:
        R.crit_("fragmented echo", f"{e!r}")


def test_control_frames(host, port, R):
    section("control frames")
    # ping -> pong (echo payload)
    try:
        ws = WS(host, port)
        token = os.urandom(16)
        ws.send_frame(WS_PING, token)
        f = ws.recv_frame()
        R.check("PING -> PONG", f["opcode"] == WS_PONG and f["payload"] == token,
                f"opcode={f['opcode']} match={f['payload']==token}")
        ws.close()
    except Exception as e:
        R.crit_("ping/pong", f"{e!r}")
    # unsolicited pong should be ignored, connection stays usable
    try:
        ws = WS(host, port)
        ws.send_frame(WS_PONG, b"unsolicited")
        _, got = ws.echo(b"still-here")
        ws.close()
        R.check("unsolicited PONG ignored", got == b"still-here")
    except Exception as e:
        R.crit_("unsolicited pong", f"{e!r}")
    # close handshake -> server should reply with a CLOSE frame
    try:
        ws = WS(host, port)
        ws.send_frame(WS_CLOSE, struct.pack("!H", 1000))
        f = ws.recv_frame(timeout=4)
        R.check("CLOSE -> CLOSE echo", f["opcode"] == WS_CLOSE,
                f"opcode={f['opcode']}", critical=False)
        ws.close()
    except (ConnectionError, socket.timeout) as e:
        R.warn_("CLOSE handshake", f"no close frame back ({e!r})")


def test_masking(host, port, R):
    section("RFC framing rules")
    # client frames MUST be masked; an unmasked frame should fail the connection
    try:
        ws = WS(host, port)
        ws.send_frame(WS_BIN, app_frame(OP_ECHO, 1, b"unmasked"), mask=False)
        try:
            f = ws.recv_frame(timeout=3)
            if f["opcode"] == WS_CLOSE:
                R.ok_("unmasked frame rejected", "server closed (RFC-correct)")
            else:
                R.warn_("unmasked frame accepted", "server processed an unmasked client frame")
        except (ConnectionError, socket.timeout):
            R.ok_("unmasked frame rejected", "connection dropped")
        ws.close()
    except Exception as e:
        R.warn_("unmasked frame", f"{e!r}")
    # reserved bits set with no extension -> protocol error expected
    try:
        ws = WS(host, port)
        ws.send_frame(WS_BIN, app_frame(OP_ECHO, 1, b"rsv"), rsv=0x4)
        try:
            f = ws.recv_frame(timeout=3)
            R.check("reserved-bit frame rejected", f["opcode"] == WS_CLOSE,
                    f"opcode={f['opcode']}", critical=False)
        except (ConnectionError, socket.timeout):
            R.ok_("reserved-bit frame rejected", "connection dropped")
        ws.close()
    except Exception as e:
        R.warn_("reserved bits", f"{e!r}")
    # invalid opcode (0xB is reserved) -> protocol error expected
    try:
        ws = WS(host, port)
        ws.send_frame(0xB, b"x")
        try:
            f = ws.recv_frame(timeout=3)
            R.check("invalid opcode rejected", f["opcode"] == WS_CLOSE,
                    f"opcode={f['opcode']}", critical=False)
        except (ConnectionError, socket.timeout):
            R.ok_("invalid opcode rejected", "connection dropped")
        ws.close()
    except Exception as e:
        R.warn_("invalid opcode", f"{e!r}")


def test_partial_delivery(host, port, R):
    section("transport robustness")
    # send a valid frame one byte at a time
    frame_payload = app_frame(OP_ECHO, 1, b"dribble-" + os.urandom(40))
    n = len(frame_payload)
    mk = os.urandom(4)
    masked = bytes(frame_payload[i] ^ mk[i % 4] for i in range(n))
    raw = bytes([0x80 | WS_BIN, 0x80 | n]) + mk + masked
    try:
        ws = WS(host, port)
        for byte in raw:
            ws.sock.sendall(bytes([byte]))
            time.sleep(0.001)
        f = ws.recv_app()
        op, fl, mid, plen = HDR.unpack(f["payload"][:8])
        ws.close()
        R.check("byte-at-a-time frame", f["payload"][8:8 + plen] == frame_payload[8:])
    except Exception as e:
        R.crit_("byte-at-a-time frame", f"{e!r}")
    # pipelined: 3 complete frames in a single write
    try:
        ws = WS(host, port)
        blob = b""
        for i in range(3):
            p = app_frame(OP_ECHO, i + 1, f"pipe{i}".encode())
            mk = os.urandom(4)
            blob += bytes([0x80 | WS_BIN, 0x80 | len(p)]) + mk + \
                    bytes(p[j] ^ mk[j % 4] for j in range(len(p)))
        ws.sock.sendall(blob)
        got = []
        for _ in range(3):
            f = ws.recv_app()
            op, fl, mid, plen = HDR.unpack(f["payload"][:8])
            got.append(f["payload"][8:8 + plen])
        ws.close()
        R.check("pipelined frames", got == [b"pipe0", b"pipe1", b"pipe2"], str(got))
    except Exception as e:
        R.crit_("pipelined frames", f"{e!r}")


def test_concurrency(host, port, R):
    section("concurrency")
    N, M = 40, 25
    errors = []
    def worker(wid):
        try:
            ws = WS(host, port)
            for j in range(M):
                payload = f"w{wid}-m{j}-".encode() + os.urandom(200)
                _, got = ws.echo(payload, mid=(j % 65535) + 1)
                if got != payload:
                    errors.append(f"w{wid}: mismatch at {j}"); break
            ws.close()
        except Exception as e:
            errors.append(f"w{wid}: {e!r}")
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(N)]
    t0 = time.time()
    for t in threads: t.start()
    for t in threads: t.join()
    dt = time.time() - t0
    R.check(f"{N} conns x {M} echoes", not errors,
            f"{N*M} msgs in {dt:.2f}s" if not errors else f"{len(errors)} errs e.g. {errors[0]}")

    # rapid connect/disconnect churn
    try:
        for _ in range(200):
            ws = WS(host, port); ws.close()
        ws = WS(host, port); _, got = ws.echo(b"survived"); ws.close()
        R.check("200x connect/disconnect churn", got == b"survived")
    except Exception as e:
        R.crit_("connect/disconnect churn", f"{e!r}")


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/"
    u = urlparse(url)
    host, port = u.hostname or "127.0.0.1", u.port or 8080
    R = Results()
    print(f"== wslib adversarial harness -> {host}:{port} ==")
    for t in (test_sizes, test_over_limit, test_fragmentation, test_control_frames,
              test_masking, test_partial_delivery, test_concurrency):
        try:
            t(host, port, R)
        except Exception as e:
            R.crit_(t.__name__, f"harness error: {e!r}")
    print(f"\n== summary: {R.ok} ok, {R.warn} warn, {R.crit} critical ==")
    sys.exit(1 if R.crit else 0)


if __name__ == "__main__":
    main()
