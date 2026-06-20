#!/usr/bin/env python3
"""cellar CRUD-hook end-to-end (Phase 2 §8): before/authorize/after on the real
REST write path. The bundle (hooks_crud.lua, selected via CEL_HOOKS_FILE) hooks
`products`: before transforms/validates, authorize adds a deny gate, after writes
an audit note. Booted by run_with_server.
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
    s, b = req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})
    return (b or {}).get("token")


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<44} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar CRUD-hook harness -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    # ---- before(): transforms the input in place ----
    s, b = req("POST", "/api/products", {"name": "Widget", "sku": "CRUD-1", "price": 2}, token=admin)
    row = (b or {}).get("row", {})
    chk("create -> 201", s == 201, f"status={s}")
    chk("before transformed name (HOOKED:)", row.get("name") == "HOOKED:Widget", str(row.get("name")))
    widget_id = row.get("id")

    # ---- before(): rejects invalid input ----
    s, b = req("POST", "/api/products", {"sku": "CRUD-2", "price": 1}, token=admin)  # no name
    chk("before reject missing name -> 400", s == 400, f"status={s}")
    chk("before reject reason surfaced", "name required by hook" in (b or {}).get("message", ""),
        str(b))

    # ---- authorize(): additional deny gate (sku BLOCKED*) ----
    s, b = req("POST", "/api/products", {"name": "X", "sku": "BLOCKED-9", "price": 1}, token=admin)
    chk("authorize denies BLOCKED sku -> 403", s == 403, f"status={s}")
    # a non-blocked sku still goes through
    s, b = req("POST", "/api/products", {"name": "Fine", "sku": "OKAY-3", "price": 1}, token=admin)
    chk("non-blocked create -> 201", s == 201, f"status={s}")

    # ---- authorize() on the READ path: row-level get gate ----
    s, _ = req("GET", f"/api/products/{widget_id}", token=admin)
    chk("get normal product -> 200 (read allowed)", s == 200, f"status={s}")
    # a product whose stored name contains SECRET is hidden on GET by authorize
    s, b = req("POST", "/api/products", {"name": "SECRET", "sku": "SEC-1", "price": 1}, token=admin)
    sec_id = (b or {}).get("row", {}).get("id")   # stored name == 'HOOKED:SECRET'
    chk("create SECRET product -> 201", s == 201, f"status={s}")
    s, _ = req("GET", f"/api/products/{sec_id}", token=admin)
    chk("get SECRET product -> 403 (read authz denies)", s == 403, f"status={s}")
    # list still works (authorize('list',...) allowed)
    s, b = req("GET", "/api/products?select=id&limit=1", token=admin)
    chk("list products -> 200 (list authorize allows)", s == 200, f"status={s}")

    # ---- after(): post-commit side effect wrote audit notes ----
    s, b = req("GET", "/api/notes?select=title&order=title", token=admin)
    titles = [r.get("title") for r in (b or {}).get("rows", [])]
    chk("after wrote audit note for CRUD-1", "audit:created:CRUD-1" in titles, str(titles))
    chk("after wrote audit note for OKAY-3", "audit:created:OKAY-3" in titles, str(titles))
    # the rejected/denied creates left no audit note
    chk("no audit note for blocked/rejected",
        not any("BLOCKED" in t or "CRUD-2" in t for t in titles), str(titles))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
