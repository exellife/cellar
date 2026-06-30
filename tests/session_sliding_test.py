#!/usr/bin/env python3
"""Per-app sliding session policy (_session) end-to-end. Booted by run_with_server
with CEL_POLICY_FILE=policies.session-sliding.json (strategy=sliding, idle=3s,
absolute_max=5s). Proves the three sliding behaviors against the real HTTP path:

  1. renew-on-use: a session used within the idle window keeps living past the
     original 3s window (the fixed strategy would have dropped it at 3s);
  2. absolute cap: continuous activity still can't push a session past the 5s cap;
  3. idle drop: a session left idle longer than the window expires.

The session cache is off by default in this harness, so every request re-checks +
renews against the DB (deterministic). Requests land at 67% of the window, leaving
~1s margins against HTTP/sleep drift.
"""
import http.client, json, sys, time
from urllib.parse import urlparse

ADMIN = ("admin@cellar.dev", "s3cret-admin")
HOST = PORT = None


def req(method, path, body=None, token=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    h = {}
    if body is not None: h["Content-Type"] = "application/json"
    if token: h["Authorization"] = "Bearer " + token
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
    r = c.getresponse(); r.read(); c.close()
    return r.status


def login(creds):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    c.request("POST", "/auth/login", body=json.dumps({"email": creds[0], "password": creds[1]}),
              headers={"Content-Type": "application/json"})
    r = c.getresponse(); data = r.read(); c.close()
    try: return (json.loads(data) or {}).get("token")
    except Exception: return None


def authed(tok):
    return req("GET", "/api/products", token=tok)   # 200 for the superuser admin; non-200 once dead


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<46} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== sliding session policy -> {HOST}:{PORT} ==")
    tok = login(ADMIN)
    chk("login admin", bool(tok))

    # --- 1. renew-on-use: stay active across the 3s idle window (hits @2s) ---
    s0 = authed(tok); chk("t0 authed -> 200", s0 == 200, f"status={s0}")
    time.sleep(2.0)                    # 2s < 3s window, past the 50% renew threshold
    s1 = authed(tok); chk("renews within window -> 200", s1 == 200, f"status={s1}")
    time.sleep(2.0)                    # ~4s since login: PAST the original 3s idle window
    s2 = authed(tok)
    chk("active session alive past idle window", s2 == 200,
        f"status={s2} (fixed strategy would have expired at 3s)")

    # --- 2. absolute cap: more activity still can't push past the 5s cap ---
    time.sleep(2.0)                    # ~6s since login; last use 2s ago (not idle) -> cap, not idle
    s = authed(tok)
    chk("absolute cap kills still-active session", s != 200, f"status={s}")

    # --- 3. idle drop: a fresh session left idle past the window dies ---
    tok2 = login(ADMIN)
    chk("fresh login", bool(tok2))
    chk("fresh session authed -> 200", authed(tok2) == 200)
    time.sleep(3.6)                    # > 3s idle, no activity
    s = authed(tok2)
    chk("idle session expired", s != 200, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
