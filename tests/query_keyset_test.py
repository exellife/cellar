#!/usr/bin/env python3
"""pgforge keyset/cursor pagination end-to-end (#50).

Creates an isolated category with 7 products, then walks them with keyset
pagination (order=sku, limit=3, following next_cursor). Verifies the pages tile
the full ordered set exactly — no gaps, no duplicates — and that the last
(short) page has no next_cursor. Proves the cursor predicate works against live
Postgres (column-typed bind inference). Self-cleaning. Booted by the harness.
"""
import http.client, json, sys
from urllib.parse import urlparse, quote

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


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<42} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge keyset pagination harness -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    tag = "keyset-" + str(abs(hash((HOST, PORT))) % 10_000_000)
    s, b = req("POST", "/api/categories", {"name": tag}, token=admin)
    cat = (b or {}).get("row", {}).get("id")
    chk("create category", bool(cat), f"status={s}")

    N = 7
    want = [f"{tag}-{i:02d}" for i in range(1, N + 1)]
    pids = []
    for i, sku in enumerate(want, 1):
        s, b = req("POST", "/api/products",
                   {"name": f"{tag} {i}", "sku": sku, "price": i, "category_id": cat}, token=admin)
        pids.append((b or {}).get("row", {}).get("id"))
    chk("create 7 products", all(pids), "")

    # walk all pages with keyset (order=sku asc, limit=3)
    got = []
    page_sizes = []
    cursor = ""
    pages = 0
    while pages < 20:
        path = f"/api/products?category_id=eq.{cat}&order=sku&limit=3&cursor={quote(cursor)}"
        s, b = req("GET", path, token=admin)
        if s != 200:
            chk("page request 200", False, f"status={s}")
            break
        rows = (b or {}).get("rows") or []
        got += [r["sku"] for r in rows]
        page_sizes.append(len(rows))
        pages += 1
        nc = (b or {}).get("next_cursor")
        if not nc:
            break
        cursor = nc

    chk("pages tile the full set in order", got == want, f"{got}")
    chk("no duplicates", len(got) == len(set(got)))
    chk("page sizes 3,3,1", page_sizes == [3, 3, 1], f"{page_sizes}")
    chk("last page had no next_cursor", pages == 3)

    # an invalid cursor is a clean 400, not a crash
    s, _ = req("GET", f"/api/products?order=sku&cursor=!!!notbase64!!!", token=admin)
    chk("garbage cursor -> 400", s == 400, f"status={s}")

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
