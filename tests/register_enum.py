#!/usr/bin/env python3
"""pgforge H-5: /auth/register is not an account-enumeration oracle by default.

With the secure default (no PGF_REGISTER_AUTOLOGIN), register returns an identical
uniform 202 for a brand-new email and an already-registered one — no session
token, no distinguishing 409 — so an attacker cannot probe which emails exist.
The account is still created (the user can log in afterward).
Run as: register_enum.py ws://127.0.0.1:<port>/
"""
import http.client, json, sys, time
from urllib.parse import urlparse

HOST = PORT = None


def req(method, path, body=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    h = {"Content-Type": "application/json"} if body is not None else {}
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
    r = c.getresponse(); data = r.read(); c.close()
    try: parsed = json.loads(data)
    except Exception: parsed = None
    return r.status, parsed, data


def main():
    ok = 0; fail = 0
    def chk(n, cond, d=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {n:<42} {d}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge register-enumeration harness -> {HOST}:{PORT} ==")
    email = f"enum-{int(time.time()*1000)}@test.local"
    body = {"email": email, "password": "enum-pass-123", "role": "rider"}

    # 1. brand-new email -> uniform 202, no token
    s1, b1, raw1 = req("POST", "/auth/register", body)
    chk("new email -> 202", s1 == 202, str(s1))
    chk("new email -> no token", not (b1 or {}).get("token"), str(b1))

    # 2. SAME email again -> byte-identical response (no 409, no enumeration signal)
    s2, b2, raw2 = req("POST", "/auth/register", body)
    chk("duplicate -> same status as new", s2 == s1, f"{s2} vs {s1}")
    chk("duplicate -> no 409", s2 != 409, str(s2))
    chk("duplicate -> identical body (no oracle)", raw2 == raw1, f"{raw2!r} vs {raw1!r}")

    # 3. the account WAS created -> the user can sign in afterward
    s3, b3, _ = req("POST", "/auth/login", {"email": email, "password": "enum-pass-123"})
    chk("registered user can log in", s3 == 200 and bool((b3 or {}).get("token")), str(s3))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
