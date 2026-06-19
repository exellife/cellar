#!/usr/bin/env python3
"""cellar boolean filter tree end-to-end (#52). Exercises and/or/not/between via
the REST `where=<url-encoded JSON>` param against live Postgres, on an isolated
fixture (one category, 7 products priced 1..7). Self-cleaning. Booted by harness.
"""
import http.client, json, sys
from urllib.parse import urlparse, quote

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
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<40} {detail}")
        ok += bool(cond); fail += (not cond)

    admin = login(ADMIN)
    chk("login admin", bool(admin))
    tag = "filt-" + str(abs(hash((HOST, PORT))) % 10_000_000)
    s, b = req("POST", "/api/categories", {"name": tag}, token=admin)
    cat = (b or {}).get("row", {}).get("id")
    chk("create category", bool(cat), f"status={s}")

    pids = []
    for i in range(1, 8):
        s, b = req("POST", "/api/products",
                   {"name": f"{tag} {i}", "sku": f"{tag}-{i:02d}", "price": i, "category_id": cat}, token=admin)
        pids.append((b or {}).get("row", {}).get("id"))
    chk("create 7 products (price 1..7)", all(pids))

    def skus(where):
        path = f"/api/products?category_id=eq.{cat}&order=sku&where={quote(json.dumps(where))}"
        s, b = req("GET", path, token=admin)
        return s, [r["sku"] for r in ((b or {}).get("rows") or [])]

    def n(i): return f"{tag}-{i:02d}"

    # between
    s, got = skus({"price": {"between": [3, 5]}})
    chk("between [3,5]", s == 200 and got == [n(3), n(4), n(5)], f"{got}")
    # or
    s, got = skus({"or": [{"price": {"eq": 1}}, {"price": {"eq": 7}}]})
    chk("OR (price 1 or 7)", s == 200 and got == [n(1), n(7)], f"{got}")
    # not
    s, got = skus({"not": {"price": {"lt": 5}}})
    chk("NOT (price < 5)", s == 200 and got == [n(5), n(6), n(7)], f"{got}")
    # nested: (price>=6) OR (price<=2 AND sku ends 01) -> 6,7 plus 01
    s, got = skus({"or": [{"price": {"gte": 6}}, {"and": [{"price": {"lte": 2}}, {"sku": {"eq": n(1)}}]}]})
    chk("nested or/and", s == 200 and got == [n(1), n(6), n(7)], f"{got}")
    # flat filter + tree combine (AND): category flat + price between
    s, got = skus({"price": {"between": [2, 3]}})
    chk("flat AND tree", s == 200 and got == [n(2), n(3)], f"{got}")

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
