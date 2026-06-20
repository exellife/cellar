#!/usr/bin/env python3
"""cellar sync_pull end-to-end (Slice 1).

POST /sync/pull returns the caller's owner-scoped rows across syncable tables with
rev > since, INCLUDING tombstones (so deletions propagate), plus a safe {cursor, more}.
Drives the syncable `items` table and asserts: full pull (tombstone included), the
cursor/more, incremental pull from a cursor, pagination safety (a small page never
skips rows on re-pull), the `tables` filter, and 401 when unauthenticated. Booted by
run_with_server (which seeds `items`).
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


def pull(token, since, **kw):
    body = {"since": since}; body.update(kw)
    s, b = req("POST", "/sync/pull", body, token=token)
    return s, b


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<50} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar sync_pull harness -> {HOST}:{PORT} ==")
    tok = login(ADMIN)
    chk("login admin", bool(tok))

    # Deterministic rev sequence on the empty `items` table:
    #   a=1, b=2, c=3, update a -> 4, delete b -> 5
    ids = {}
    for nm in ("a", "b", "c"):
        s, b = req("POST", "/api/items", {"name": nm}, token=tok)
        ids[nm] = b["row"]["id"]
    req("PATCH", f"/api/items/{ids['a']}", {"name": "a2"}, token=tok)
    req("DELETE", f"/api/items/{ids['b']}", token=tok)

    # ---- full pull (since=0): every live row + the tombstone, ordered by rev ----
    s, b = pull(tok, 0)
    chk("pull -> 200", s == 200, f"status={s}")
    rows = b["changes"].get("items", [])
    by_rev = [r["rev"] for r in rows]
    chk("rows ordered by rev ASC", by_rev == sorted(by_rev), str(by_rev))
    chk("3 rows (a updated, c, b-tombstone)", len(rows) == 3, str(by_rev))
    bt = [r for r in rows if r["id"] == ids["b"]]
    chk("tombstone b included with deleted=1", len(bt) == 1 and bt[0]["deleted"] == 1, str(bt))
    chk("live rows carry deleted=0", all(r["deleted"] == 0 for r in rows if r["id"] != ids["b"]))
    chk("cursor = max rev (5), more=false", b["cursor"] == 5 and b["more"] is False, str((b["cursor"], b["more"])))

    # ---- incremental: a new write, pull from the last cursor returns only it ----
    s, b2 = req("POST", "/api/items", {"name": "d"}, token=tok)   # rev 6
    s, b = pull(tok, 5)
    rows = b["changes"].get("items", [])
    chk("incremental pull since=5 -> only 'd'", len(rows) == 1 and rows[0]["name"] == "d", str(rows))
    chk("cursor advances to 6", b["cursor"] == 6 and b["more"] is False, str((b["cursor"], b["more"])))

    # ---- pagination safety: small pages, re-pull from cursor, no row skipped ----
    seen = set(); since = 0; pages = 0
    while True:
        s, b = pull(tok, since, limit=2)
        rows = b["changes"].get("items", [])
        for r in rows: seen.add(r["rev"])
        pages += 1
        if not b["more"]:
            break
        chk(f"page {pages}: cursor advanced", b["cursor"] > since, str((since, b["cursor"])))
        since = b["cursor"]
        if pages > 10: break   # safety
    # Each row carries only its LATEST rev: a's create(1) is superseded by its
    # update(4), b's create(2) by its delete(5). So the current set is {3,4,5,6}.
    chk("paged pull saw every current rev {3,4,5,6}", seen == {3, 4, 5, 6}, str(sorted(seen)))

    # ---- `tables` filter ----
    s, b = pull(tok, 0, tables=["nope"])
    chk("tables=[nope] -> empty changes", b["changes"] == {}, str(b["changes"]))
    s, b = pull(tok, 0, tables=["items"])
    chk("tables=[items] -> has items", "items" in b["changes"])

    # ---- unauthenticated ----
    s, _ = req("POST", "/sync/pull", {"since": 0})
    chk("unauthenticated -> 401", s == 401, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
