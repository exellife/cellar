#!/usr/bin/env python3
"""Web-push subscriptions + cellar.notify end-to-end (NotifChannel slice 1). Booted by
run_with_server. Covers the /push endpoints (subscribe upsert / list / unsubscribe /
auth) and that cellar.notify enqueues a fan-out job. (Actual channel DELIVERY is
slice 2 — the web-push adapter against a mock push service.)
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
    return req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})[1] or {}


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<48} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== push subscriptions + notify -> {HOST}:{PORT} ==")
    lg = login(ADMIN); tok = lg.get("token"); uid = (lg.get("user") or {}).get("id")
    chk("login admin", bool(tok) and bool(uid))

    EP = "https://push.example.com/ep/abc123"
    # mint without auth -> 401
    s, _ = req("POST", "/push/subscribe", {"endpoint": EP, "keys": {"p256dh": "k", "auth": "a"}})
    chk("subscribe without auth -> 401", s == 401, f"status={s}")
    # subscribe
    s, b = req("POST", "/push/subscribe", {"endpoint": EP, "keys": {"p256dh": "k1", "auth": "a1"}, "ua": "Firefox"}, token=tok)
    sid = (b or {}).get("id")
    chk("subscribe -> 201 + id", s == 201 and bool(sid), f"status={s}")
    # missing keys -> 400
    s, _ = req("POST", "/push/subscribe", {"endpoint": "https://x/y"}, token=tok)
    chk("subscribe missing keys -> 400", s == 400, f"status={s}")
    # list shows it, WITHOUT key material
    s, b = req("GET", "/push/subscriptions", token=tok)
    subs = (b or {}).get("subscriptions") or []
    one = subs[0] if subs else {}
    chk("list shows the subscription", s == 200 and len(subs) == 1 and one.get("id") == sid
        and one.get("endpoint") == EP and one.get("ua") == "Firefox", str(b))
    chk("list leaks no key material", "p256dh" not in one and "auth" not in one, str(one))
    # re-subscribe same endpoint -> SAME id (upsert), still one row
    s, b = req("POST", "/push/subscribe", {"endpoint": EP, "keys": {"p256dh": "k2", "auth": "a2"}}, token=tok)
    chk("re-subscribe same endpoint upserts (same id)", s == 201 and (b or {}).get("id") == sid, str(b))
    s, b = req("GET", "/push/subscriptions", token=tok)
    chk("still one subscription after upsert", len((b or {}).get("subscriptions") or []) == 1)
    # unsubscribe by endpoint, then it's gone
    s, _ = req("POST", "/push/unsubscribe", {"endpoint": EP}, token=tok)
    chk("unsubscribe -> 200", s == 200, f"status={s}")
    s, b = req("GET", "/push/subscriptions", token=tok)
    chk("list empty after unsubscribe", len((b or {}).get("subscriptions") or []) == 0)
    s, _ = req("POST", "/push/unsubscribe", {"id": "nope"}, token=tok)
    chk("unsubscribe unknown -> 404", s == 404, f"status={s}")

    # cellar.notify enqueues a fan-out job (delivery itself is slice 2)
    s, b = req("POST", "/rpc/notify", {"user_id": uid, "title": "Hi", "body": "there"}, token=tok)
    chk("cellar.notify -> enqueued", s == 200 and (b or {}).get("result", {}).get("ok") is True, str(b))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
