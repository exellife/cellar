#!/usr/bin/env python3
"""pgforge richer-read test: relationship embedding + exact count (OP_DB_QUERY).

Exercises the demo product catalog over REST: embed a to-one relation
(products -> categories via products.category_id), a to-many relation
(categories -> products via the reverse FK), and ?count=exact (the unpaginated
total). Creates its own isolated category + products so the assertions are
deterministic regardless of what other suites left in the shared DB. Booted by
the harness, which passes ws://host:port/ as argv[1].
"""
import http.client, json, sys
from urllib.parse import urlparse

ADMIN = ("admin@pgforge.dev", "s3cret-admin")
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
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<40} {detail}")
        ok += bool(cond); fail += (not cond)
        return cond

    print(f"== pgforge richer-read harness -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    # ---- isolated fixture: one category + two products under it ----
    tag = "embed-" + str(abs(hash((HOST, PORT))) % 10_000_000)
    s, b = req("POST", "/api/categories", {"name": tag, "description": "embed test"}, token=admin)
    cat_id = (b or {}).get("row", {}).get("id")
    chk("create category", s in (200, 201) and bool(cat_id), f"status={s}")

    pids = []
    for i in (1, 2):
        s, b = req("POST", "/api/products",
                   {"name": f"{tag}-p{i}", "sku": f"{tag}-{i}", "price": i, "category_id": cat_id},
                   token=admin)
        pids.append((b or {}).get("row", {}).get("id"))
        chk(f"create product {i}", s in (200, 201) and bool(pids[-1]), f"status={s}")

    # ---- to-one: each product carries its parent category object ----
    s, b = req("GET", f"/api/products?category_id=eq.{cat_id}&embed=categories", token=admin)
    rows = (b or {}).get("rows") or []
    chk("to-one: status + 2 rows", s == 200 and len(rows) == 2, f"status={s} n={len(rows)}")
    embedded_ok = rows and all(isinstance(r.get("categories"), dict)
                               and r["categories"].get("name") == tag for r in rows)
    chk("to-one: embedded category matches", bool(embedded_ok))

    # ---- to-many: the category carries its products array ----
    s, b = req("GET", f"/api/categories?id=eq.{cat_id}&embed=products", token=admin)
    rows = (b or {}).get("rows") or []
    cat = rows[0] if rows else {}
    kids = cat.get("products")
    chk("to-many: status + 1 category", s == 200 and len(rows) == 1, f"status={s}")
    chk("to-many: products is a 2-element array",
        isinstance(kids, list) and len(kids) == 2,
        f"n={len(kids) if isinstance(kids, list) else 'n/a'}")
    chk("to-many: child skus correct",
        isinstance(kids, list) and {k.get("sku") for k in kids} == {f"{tag}-1", f"{tag}-2"})

    # ---- exact count: total ignores the page limit ----
    s, b = req("GET", f"/api/products?category_id=eq.{cat_id}&count=exact&limit=1", token=admin)
    rows = (b or {}).get("rows") or []
    chk("count: one row on the page", s == 200 and len(rows) == 1, f"n={len(rows)}")
    chk("count: exact total is 2", (b or {}).get("total") == 2, f"total={(b or {}).get('total')}")

    # ---- count honors the filter (unique sku -> exactly 1) ----
    s, b = req("GET", f"/api/products?sku=eq.{tag}-1&count=exact", token=admin)
    chk("count: filtered total is 1", s == 200 and (b or {}).get("total") == 1,
        f"total={(b or {}).get('total')}")

    # ---- embedding is bounded to the FK graph: a bogus relation is a 400 ----
    s, b = req("GET", "/api/products?embed=not_a_relation", token=admin)
    chk("bad relation -> 400", s == 400, f"status={s}")
    # internal tables are not exposed, so they can't be embedded either
    s, b = req("GET", "/api/products?embed=pgf_users", token=admin)
    chk("internal table not embeddable -> 400", s == 400, f"status={s}")

    # ---- clean up the fixture so the shared DB stays pristine for other suites
    # (smoke/rest assert categories count == 2). Products first (FK), then category.
    for pid in pids:
        if pid: req("DELETE", f"/api/products/{pid}", token=admin)
    if cat_id: req("DELETE", f"/api/categories/{cat_id}", token=admin)
    s, b = req("GET", f"/api/categories?id=eq.{cat_id}", token=admin)
    chk("cleanup: fixture category removed", s == 200 and len((b or {}).get("rows") or []) == 0)

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
