#!/usr/bin/env python3
"""cellar auth rate-limit end-to-end: hammer /auth/login, expect 429.

Run against a server booted with CEL_AUTH_RATELIMIT=3/60: the first 3 attempts
from this IP are allowed (401 invalid creds — the limiter let them reach auth),
the rest are throttled (429). Run as: rate_limit_check.py ws://127.0.0.1:<port>/
"""
import http.client, json, sys
from urllib.parse import urlparse

HOST = PORT = None


def login_code():
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    c.request("POST", "/auth/login",
              body=json.dumps({"email": "x@y.z", "password": "wrong"}),
              headers={"Content-Type": "application/json"})
    r = c.getresponse(); r.read(); c.close()
    return r.status


def api_code():
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    c.request("GET", "/api/products")
    r = c.getresponse(); r.read(); c.close()
    return r.status


def main():
    ok = 0; fail = 0
    def chk(name, cond, d=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<34} {d}")
        ok += bool(cond); fail += (not cond)

    # ---- auth throttle (its own bucket) ----
    codes = [login_code() for _ in range(6)]
    print("  auth codes:", codes)
    chk("auth: first 3 reach auth (401)", codes[:3] == [401, 401, 401], str(codes[:3]))
    chk("auth: subsequent throttled (429)", all(c == 429 for c in codes[3:]), str(codes[3:]))

    # ---- data-API throttle (independent bucket; CEL_API_RATELIMIT=3/60) ----
    api = [api_code() for _ in range(6)]
    print("  api codes:", api)
    chk("api: first 3 reach the API (not 429)", all(c != 429 for c in api[:3]), str(api[:3]))
    chk("api: subsequent throttled (429)", all(c == 429 for c in api[3:]), str(api[3:]))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
