# wslib robustness audit

Adversarial testing of the vendored `lib/wslib` (RacyTech homemade WebSocket lib),
driven through pgforge's `OP_ECHO` by `tests/wslib_test.py` (a raw-socket client with
full frame control). Run via CTest: `ctest --test-dir build-cmake -R wslib_adversarial`.

> **Historical.** This documents the original adversarial audit of the vendored wslib.
> The WebSocket transport was since extracted into the **portico** library (sibling repo),
> which is now the canonical transport pgforge consumes; these fixes (and more found later)
> live there. See portico's README for the current state.

## Bugs found & fixed

### 1. 🔴 Critical — 16 KB receive cap (couldn't receive frames > 16 KB)
`recv_buffer_capacity` was fixed at 16384 (`ws_connection.c:217`) and never grew.
Despite `max_message_size = 1 MB`, any frame ≥ ~16 KB hit "Receive buffer full" and
the connection was closed. For pgforge this would silently break large JSON writes
and bulk operations.
**Fix** (`ws_connection_handler.c`): rewrote `handle_websocket_frames` as a read-loop
that drains the socket to `EAGAIN` (correct for edge-triggered epoll) and grows the
receive buffer geometrically on demand up to `max_message_size + overhead`. Over-limit
frames are now rejected gracefully without crashing.

### 2. 🔴 Critical — split frame header dropped the connection
`ws_parse_frame_header` returned `-1` (error) for incomplete headers
(`len < 2/4/10`, missing mask key) — indistinguishable from a real protocol error.
A header split across TCP reads (common under load; guaranteed for slow/partial
delivery) caused "Failed to parse" → connection close.
**Fix** (`ws_frame.c`): those cases now return `0` ("need more data"); `ws_parse_frame`
treats a `0` header result as incomplete. `-1` is reserved for genuine protocol errors.

### 3. 🟡 RFC — unmasked client frames were accepted
RFC 6455 §5.1 requires all client→server frames be masked; wslib processed unmasked
frames anyway.
**Fix** (`ws_frame.c`): reject unmasked frames as a protocol error (connection closed).

### 4. ⚪ Cosmetic — clean disconnect logged as an error
The CLOSE handler returned `-1`, conflated with real failures, so every normal
disconnect logged "Failed to process WebSocket frame on fd=N" — masking real errors.
**Fix** (`ws_connection_handler.c`): CLOSE now returns a distinct
`WS_FRAME_CLOSE_REQUESTED` sentinel; the read-loop treats it as a clean close (no
error log) while still closing the connection.

Also fixed in passing: a latent double-`memmove` in the old frame loop that could
corrupt a buffer holding complete frame(s) followed by a trailing partial frame.

## Coverage (all green: 34 ok, 0 warn, 0 critical)

- **payload sizes**: 0,1,2,125,126,127,128 and the 1K/4K/16K buffer seams and
  WS length-encoding boundaries (126, 65535/65536) up to the 1 MB cap
- **over-limit** (2 MB): rejected gracefully, server stays alive
- **fragmentation**: 3-way continuation frames reassemble
- **control frames**: ping→pong, unsolicited pong ignored, close handshake
- **RFC framing**: unmasked / reserved-bit / invalid-opcode all rejected
- **transport**: byte-at-a-time delivery, pipelined frames in one write
- **concurrency**: 40 conns × 25 echoes (1000 msgs), 200× connect/disconnect churn

## Known not-yet-addressed

- (Transport is now the portico library; the old upstream wslib is retired.)
- Catalog/echo paths only; no TLS (wss) testing (pgforge terminates TLS elsewhere).
