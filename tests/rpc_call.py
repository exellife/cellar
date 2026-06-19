#!/usr/bin/env python3
"""pgforge RPC end-to-end (#54): call a whitelisted SQL function over REST.

Boots (via the harness) against a DB with rpc_add(a,b) defined and a policy that
whitelists it for admin only. Proves: an admin call returns the function result
(typed), a non-whitelisted role is denied, and a non-whitelisted function name is
unreachable even by admin (the whitelist is the security boundary).
Run as: rpc_call.py ws://127.0.0.1:<port>/
"""
import http.client, json, sys
from urllib.parse import urlparse

HOST = PORT = None


def req(method, path, body=None, token=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    headers = {}
    if body is not None: headers["Content-Type"] = "application/json"
    if token: headers["Authorization"] = "Bearer " + token
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=headers)
    resp = c.getresponse(); data = resp.read(); c.close()
    try: parsed = json.loads(data)
    except Exception: parsed = None
    return resp.status, parsed


def login(email, pw):
    _s, b = req("POST", "/auth/login", {"email": email, "password": pw})
    return (b or {}).get("token")


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<32} {detail}")
        ok += bool(cond); fail += (not cond)

    admin = login("admin@pgforge.dev", "s3cret-admin")
    chk("admin login", bool(admin))

    # whitelisted function, allowed role -> result
    s, b = req("POST", "/rpc/rpc_add", {"a": 2, "b": 40}, token=admin)
    chk("rpc_add status 200", s == 200, str(s))
    result = (b or {}).get("result")
    chk("result is a row list", isinstance(result, list) and len(result) == 1, str(result))
    if isinstance(result, list) and result:
        chk("rpc_add == 42 (typed int)", result[0].get("rpc_add") == 42, str(result[0]))

    # a non-whitelisted role is denied
    editor = login("editor@pgforge.dev", "editor-pw")
    s, b = req("POST", "/rpc/rpc_add", {"a": 1, "b": 1}, token=editor)
    chk("editor denied 403", s == 403, str(s))

    # a non-whitelisted function is unreachable even for admin (whitelist = boundary)
    s, b = req("POST", "/rpc/pg_sleep", {"seconds": 0}, token=admin)
    chk("unlisted fn 403", s == 403, str(s))

    # unauthenticated -> denied
    s, b = req("POST", "/rpc/rpc_add", {"a": 1, "b": 1})
    chk("unauthenticated denied", s in (401, 403), str(s))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
