#!/usr/bin/env python3
"""Device-token (PIN fast-login) end-to-end. Booted by run_with_server with
CEL_POLICY_FILE=policies.device-tokens.json (device_ttl_seconds set → feature on).

Proves the loop: mint (Bearer) → exchange for a session (public) → the session
works → the token is reusable (static) → list → revoke → revoked token rejected →
list empty → garbage token rejected → unknown-id revoke 404.
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

    print(f"== device tokens -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    # mint
    s, b = req("POST", "/auth/device", {"label": "My Phone"}, token=admin)
    res = b or {}
    dev, devid = res.get("device_token"), res.get("id")
    chk("mint -> 201 + token + id", s == 201 and bool(dev) and bool(devid), f"status={s}")

    # exchange for a session, and that session works
    s, b = req("POST", "/auth/session/from-device", {"device_token": dev})
    sess = (b or {}).get("token")
    chk("exchange -> session token", s == 200 and bool(sess), f"status={s}")
    chk("exchanged session is usable", req("GET", "/api/products", token=sess)[0] == 200)

    # static/reusable: exchanging again still works
    s, b = req("POST", "/auth/session/from-device", {"device_token": dev})
    chk("device token is reusable", s == 200 and bool((b or {}).get("token")), f"status={s}")

    # list shows it (no token value)
    s, b = req("GET", "/auth/devices", token=admin)
    devs = (b or {}).get("devices") or []
    chk("list shows the device", s == 200 and len(devs) == 1 and devs[0].get("id") == devid
        and devs[0].get("label") == "My Phone" and "device_token" not in devs[0] and "token" not in devs[0],
        str(b))

    # revoke, then the token no longer works and drops off the list
    s, b = req("POST", "/auth/devices/revoke", {"id": devid}, token=admin)
    chk("revoke -> 200", s == 200, f"status={s}")
    s, b = req("POST", "/auth/session/from-device", {"device_token": dev})
    chk("revoked token rejected", s == 401, f"status={s}")
    s, b = req("GET", "/auth/devices", token=admin)
    chk("list empty after revoke", s == 200 and len((b or {}).get("devices") or []) == 0)

    # garbage token + unknown revoke id
    s, _ = req("POST", "/auth/session/from-device", {"device_token": "deadbeef"})
    chk("garbage device token -> 401", s == 401, f"status={s}")
    s, _ = req("POST", "/auth/devices/revoke", {"id": "nope"}, token=admin)
    chk("revoke unknown id -> 404", s == 404, f"status={s}")

    # minting requires auth
    s, _ = req("POST", "/auth/device", {"label": "x"})
    chk("mint without auth -> 401", s == 401, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
