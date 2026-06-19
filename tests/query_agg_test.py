#!/usr/bin/env python3
"""pgforge group-by aggregates end-to-end (#54). Two isolated categories with
priced products; group by category and check count/sum/avg/min/max, a no-group
totals query, and rejection of an unknown aggregate function. Self-cleaning.
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
    r = c.getresponse(); data = r.read(); c.close()
    try: parsed = json.loads(data)
    except Exception: parsed = None
    return r.status, parsed


def login(creds):
    s, b = req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})
    return (b or {}).get("token")


def num(v):
    try: return float(str(v))
    except Exception: return None


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<42} {detail}")
        ok += bool(cond); fail += (not cond)

    admin = login(ADMIN)
    chk("login admin", bool(admin))
    tag = "agg-" + str(abs(hash((HOST, PORT))) % 10_000_000)

    cats = {}
    pids = []
    for name, prices in (("A", [10, 20, 30]), ("B", [5, 15])):
        s, b = req("POST", "/api/categories", {"name": f"{tag}-{name}"}, token=admin)
        cid = (b or {}).get("row", {}).get("id")
        cats[name] = cid
        for i, pr in enumerate(prices):
            s, b = req("POST", "/api/products",
                       {"name": f"{tag} {name}{i}", "sku": f"{tag}-{name}-{i}", "price": pr, "category_id": cid},
                       token=admin)
            pids.append((b or {}).get("row", {}).get("id"))
    chk("create 2 categories + 5 products", all(cats.values()) and all(pids))

    A, B = cats["A"], cats["B"]

    # group by category: count + sum + avg
    s, b = req("GET", f"/api/products?category_id=in.{A},{B}&group=category_id&aggregate=count,sum:price,avg:price",
               token=admin)
    rows = (b or {}).get("rows") or []
    by = {r.get("category_id"): r for r in rows}
    chk("two groups returned", s == 200 and len(rows) == 2, f"n={len(rows)}")
    chk("A: count 3, sum 60, avg 20",
        A in by and num(by[A]["count"]) == 3 and num(by[A]["sum_price"]) == 60 and num(by[A]["avg_price"]) == 20,
        str(by.get(A)))
    chk("B: count 2, sum 20, avg 10",
        B in by and num(by[B]["count"]) == 2 and num(by[B]["sum_price"]) == 20 and num(by[B]["avg_price"]) == 10,
        str(by.get(B)))

    # no-group totals over one category: count + min + max
    s, b = req("GET", f"/api/products?category_id=eq.{A}&aggregate=count,min:price,max:price", token=admin)
    rows = (b or {}).get("rows") or []
    tot = rows[0] if rows else {}
    chk("totals: count 3, min 10, max 30",
        s == 200 and len(rows) == 1 and num(tot.get("count")) == 3
        and num(tot.get("min_price")) == 10 and num(tot.get("max_price")) == 30, str(tot))

    # an unknown aggregate function is rejected, not run
    s, _ = req("GET", f"/api/products?aggregate=median:price", token=admin)
    chk("unknown aggregate fn -> 400", s == 400, f"status={s}")
    # a sum without a column is rejected
    s, _ = req("GET", f"/api/products?aggregate=sum", token=admin)
    chk("sum without column -> 400", s == 400, f"status={s}")

    # cleanup
    for pid in pids:
        if pid: req("DELETE", f"/api/products/{pid}", token=admin)
    for cid in cats.values():
        if cid: req("DELETE", f"/api/categories/{cid}", token=admin)

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
