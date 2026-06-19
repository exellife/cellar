#!/usr/bin/env python3
"""cellar nested/dotted embed end-to-end (#53). Uses the demo's circular relation
(categories <- products -> categories): embed=products.categories nests two
levels (to-many then to-one), and embed=categories.products nests to-one then
to-many. Verifies authz re-runs per level by construction. Self-cleaning.
"""
import http.client, json, sys
from urllib.parse import urlparse

ADMIN = ("admin@cellar.dev", "s3cret-admin")
HOST = PORT = None


def req(method, path, body=None, token=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    headers = {}
    if body is not None: headers["Content-Type"] = "application/json"
    if token: headers["Authorization"] = "Bearer " + token
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=headers)
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

    admin = login(ADMIN)
    chk("login admin", bool(admin))
    tag = "nest-" + str(abs(hash((HOST, PORT))) % 10_000_000)
    s, b = req("POST", "/api/categories", {"name": tag}, token=admin)
    cat = (b or {}).get("row", {}).get("id")
    chk("create category", bool(cat), f"status={s}")
    pids = []
    for i in range(1, 4):
        s, b = req("POST", "/api/products",
                   {"name": f"{tag} {i}", "sku": f"{tag}-{i}", "price": i, "category_id": cat}, token=admin)
        pids.append((b or {}).get("row", {}).get("id"))
    chk("create 3 products", all(pids))

    # to-many then to-one: category -> products -> each product's category (== this one)
    s, b = req("GET", f"/api/categories?id=eq.{cat}&embed=products.categories", token=admin)
    rows = (b or {}).get("rows") or []
    c0 = rows[0] if rows else {}
    prods = c0.get("products") or []
    chk("level1: category has its 3 products", s == 200 and len(prods) == 3, f"n={len(prods)}")
    nested_ok = prods and all(isinstance(p.get("categories"), dict) and p["categories"].get("id") == cat
                              for p in prods)
    chk("level2: each product nests its category back", bool(nested_ok))

    # to-one then to-many: product -> its category -> that category's products
    s, b = req("GET", f"/api/products?id=eq.{pids[0]}&embed=categories.products", token=admin)
    rows = (b or {}).get("rows") or []
    p0 = rows[0] if rows else {}
    pcat = p0.get("categories") or {}
    chk("level1: product nests its category", s == 200 and pcat.get("id") == cat, f"status={s}")
    chk("level2: that category nests all 3 products",
        isinstance(pcat.get("products"), list) and len(pcat["products"]) == 3,
        f"n={len(pcat.get('products') or [])}")

    # depth cap: an absurdly deep path is rejected, not run
    deep = "categories.products.categories.products.categories"
    s, _ = req("GET", f"/api/products?id=eq.{pids[0]}&embed={deep}", token=admin)
    chk("over-deep embed -> 400", s == 400, f"status={s}")

    # cleanup
    for pid in pids:
        if pid: req("DELETE", f"/api/products/{pid}", token=admin)
    if cat: req("DELETE", f"/api/categories/{cat}", token=admin)

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
