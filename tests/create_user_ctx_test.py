#!/usr/bin/env python3
"""cellar.create_user context guard (regression for the review's HIGH deadlock +
escalation findings). Booted by run_with_server with CEL_HOOKS_FILE=hooks_create_user_ctx.lua.

- The rpc path (no write lock) can mint a non-superuser account.
- A before() hook calling create_user (UNDER the app write lock) is REFUSED with a
  clear error — and the request RETURNS instead of deadlocking the worker. If the
  guard regressed, this request would hang and the 10s timeout would fail the test.
"""
import http.client, json, sys
from urllib.parse import urlparse

ADMIN = ("admin@cellar.dev", "s3cret-admin")
HOST = PORT = None


def req(method, path, body=None, token=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    h = {}
    if body is not None: h["Content-Type"] = "application/json"
    if token: h["Authorization"] = "Bearer " + token
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
    r = c.getresponse(); data = r.read(); c.close()
    try: parsed = json.loads(data)
    except Exception: parsed = None
    return r.status, parsed


def login(creds):
    return (req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})[1] or {}).get("token")


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<46} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== create_user context guard -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    # SAFE: rpc path holds no write lock → create_user works
    s, b = req("POST", "/rpc/make_user",
               {"email": "rpc-made@ctx.dev", "password": "a-password-1", "role": "editor"}, token=admin)
    chk("create_user from rpc -> id", s == 200 and bool((b or {}).get("result", {}).get("id")), str(b))

    # GUARDED: before() calls create_user under the write lock. It must be refused
    # (rejecting the write) and RETURN — a regressed guard would deadlock + time out.
    s, b = req("POST", "/api/products",
               {"name": "Trip", "sku": "TRIGGER-CREATE", "price": 1.0}, token=admin)
    msg = (b or {}).get("message", "") or json.dumps(b)
    chk("before() create_user refused (no deadlock)", s == 400 and "rpc hook" in msg, f"status={s} msg={msg}")

    # server is alive after (not deadlocked): another request returns normally
    s, b = req("POST", "/rpc/make_user",
               {"email": "after@ctx.dev", "password": "a-password-2", "role": "editor"}, token=admin)
    chk("server alive after the guarded call", s == 200 and bool((b or {}).get("result", {}).get("id")), str(b))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
