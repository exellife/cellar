#!/usr/bin/env python3
"""cellar sync Slice 3 — idempotent retry + per-device cursor (end-to-end).

Over the syncable `items` table:
  - a push with a `mutation_id`, re-pushed, is a no-op (deduped, no second rev bump);
  - the exact audit lost-update is prevented: A's retry can't clobber B's interleaved write;
  - `/sync/pull` records the device's DURABLE cursor (its `since`) in _sync_devices,
    advancing monotonically (a re-pull from a lower `since` can't rewind it).
Peeks the db directly via CEL_DATA_DB (exported by run_with_server).
"""
import http.client, json, os, sqlite3, sys
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


def push(token, muts):
    return req("POST", "/sync/push", {"mutations": muts}, token=token)


def peek(sql):
    db = os.environ.get("CEL_DATA_DB")
    if not db: return None
    con = sqlite3.connect(db)
    try: return con.execute(sql).fetchone()
    finally: con.close()


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<52} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar sync dedup/device harness -> {HOST}:{PORT} ==")
    tok = login(ADMIN)
    chk("login admin", bool(tok))

    # ---- idempotent retry: same mutation_id is a no-op on re-push ----
    s, b = push(tok, [{"op": "put", "table": "items", "id": "x", "mutation_id": "M1", "values": {"name": "v1"}}])
    r1 = b["results"][0]
    chk("first push applied", r1["status"] == "applied", str(r1))
    rev1 = r1["rev"]
    s, b = push(tok, [{"op": "put", "table": "items", "id": "x", "mutation_id": "M1", "values": {"name": "v1"}}])
    r2 = b["results"][0]
    chk("retry is deduped (no re-apply)", r2.get("deduped") is True and r2["rev"] == rev1, str(r2))

    # ---- the audit lost-update: A's retry must not clobber B's interleaved write ----
    push(tok, [{"op": "put", "table": "items", "id": "y", "mutation_id": "My", "values": {"name": "y0"}}])
    yrev = peek("SELECT rev FROM items WHERE id='y'")[0]
    push(tok, [{"op": "put", "table": "items", "id": "y", "mutation_id": "M-A", "base_rev": yrev, "values": {"name": "A"}}])
    arev = peek("SELECT rev FROM items WHERE id='y'")[0]
    # B writes meanwhile (knows the post-A rev)
    push(tok, [{"op": "put", "table": "items", "id": "y", "mutation_id": "M-B", "base_rev": arev, "values": {"name": "B"}}])
    bname, brev = peek("SELECT name, rev FROM items WHERE id='y'")
    chk("B's write landed (name=B)", bname == "B", f"name={bname} rev={brev}")
    # A retries its lost push (same M-A) — must dedupe, NOT re-apply over B
    s, b = push(tok, [{"op": "put", "table": "items", "id": "y", "mutation_id": "M-A", "base_rev": yrev, "values": {"name": "A"}}])
    chk("A retry deduped", b["results"][0].get("deduped") is True, str(b["results"][0]))
    bname2, brev2 = peek("SELECT name, rev FROM items WHERE id='y'")
    chk("B's update SURVIVES A's retry (no lost update)", bname2 == "B" and brev2 == brev, f"name={bname2} rev={brev2}")

    # ---- a mutation WITHOUT mutation_id is not deduped (back-compat) ----
    push(tok, [{"op": "put", "table": "items", "id": "z", "values": {"name": "z0"}}])
    zrev = peek("SELECT rev FROM items WHERE id='z'")[0]
    push(tok, [{"op": "put", "table": "items", "id": "z", "base_rev": zrev, "values": {"name": "z1"}}])
    zrev2 = peek("SELECT rev FROM items WHERE id='z'")[0]
    chk("no mutation_id -> applies normally (rev advanced)", zrev2 > zrev, f"{zrev}->{zrev2}")

    # ---- per-device cursor: pull records `since`, advances monotonically ----
    req("POST", "/sync/pull", {"since": 0, "device_id": "d1"}, token=tok)
    chk("device cursor recorded at since=0", peek("SELECT cursor FROM _sync_devices WHERE device_id='d1'")[0] == 0)
    req("POST", "/sync/pull", {"since": 3, "device_id": "d1"}, token=tok)
    chk("device cursor advances to 3", peek("SELECT cursor FROM _sync_devices WHERE device_id='d1'")[0] == 3)
    req("POST", "/sync/pull", {"since": 1, "device_id": "d1"}, token=tok)
    chk("a lower since can't rewind it (stays 3)", peek("SELECT cursor FROM _sync_devices WHERE device_id='d1'")[0] == 3)

    # ---- audit fixes: empty-string mutation_id is NOT a shared dedup key ----
    push(tok, [{"op": "put", "table": "items", "id": "e1", "mutation_id": "", "values": {"name": "e1"}}])
    push(tok, [{"op": "put", "table": "items", "id": "e2", "mutation_id": "", "values": {"name": "e2"}}])
    chk("empty mutation_id doesn't dedup distinct rows",
        peek("SELECT count(*) FROM items WHERE id IN ('e1','e2')")[0] == 2)

    # ---- a mutation_id reused for a DIFFERENT row applies (no silent drop) ----
    push(tok, [{"op": "put", "table": "items", "id": "r1", "mutation_id": "DUP", "values": {"name": "r1"}}])
    push(tok, [{"op": "put", "table": "items", "id": "r2", "mutation_id": "DUP", "values": {"name": "r2"}}])
    chk("reused mutation_id on a different id still applies (not silently dropped)",
        peek("SELECT count(*) FROM items WHERE id IN ('r1','r2')")[0] == 2)

    # ---- _sync_devices is user-scoped: same device_id under two users -> two rows ----
    ed = login(("editor@cellar.dev", "editor-pw"))
    req("POST", "/sync/pull", {"since": 0, "device_id": "shared"}, token=tok)
    req("POST", "/sync/pull", {"since": 0, "device_id": "shared"}, token=ed)
    chk("same device_id under two users -> two rows (user-scoped, no hijack)",
        peek("SELECT count(*) FROM _sync_devices WHERE device_id='shared'")[0] == 2)

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
