#!/usr/bin/env python3
"""pgforge richer-read embedding AUTHZ test: an embedded relation is read on the
caller's behalf and must pass the SAME read policy a direct read would.

Booted with config/policies.embed-authz.example.json, where `categories` is
admin-only. A viewer can read products but must NOT be able to embed categories
(that would leak rows the viewer can't see directly); an admin (superuser) can.
Booted by the harness, which passes ws://host:port/ as argv[1].
"""
import http.client, json, sys
from urllib.parse import urlparse

ADMIN  = ("admin@pgforge.dev",  "s3cret-admin")
VIEWER = ("viewer@pgforge.dev", "viewer-pw")
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


def login(creds):
    s, b = req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})
    return (b or {}).get("token")


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<44} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge embedding-authz harness -> {HOST}:{PORT} ==")
    admin, viewer = login(ADMIN), login(VIEWER)
    chk("login admin + viewer", bool(admin) and bool(viewer))

    # the fixture's restriction is active: viewer cannot read categories directly
    s, _ = req("GET", "/api/categories", token=viewer)
    chk("viewer GET /api/categories -> 403", s == 403, f"status={s}")
    # but viewer CAN read the base table
    s, _ = req("GET", "/api/products", token=viewer)
    chk("viewer GET /api/products -> 200", s == 200, f"status={s}")

    # THE assertion: embedding a forbidden relation is refused (no leak via embed)
    s, _ = req("GET", "/api/products?embed=categories", token=viewer)
    chk("viewer embed categories -> 403", s == 403, f"status={s}")

    # admin (superuser) may embed it
    s, b = req("GET", "/api/products?embed=categories&limit=1", token=admin)
    rows = (b or {}).get("rows") or []
    chk("admin embed categories -> 200", s == 200, f"status={s}")
    chk("admin sees the embedded key", bool(rows) and "categories" in rows[0])

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
