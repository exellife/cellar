#!/usr/bin/env python3
"""cellar offline-first sync substrate end-to-end (Slice 0: T1-T5).

Drives the syncable `items` table (id, name, rev, deleted) over the REST API and
asserts the engine's rev + tombstone invariants:
  - create/update stamp a monotonic, engine-owned `rev`; client-supplied rev/deleted
    are ignored;
  - DELETE is a soft-delete (the row physically remains as a tombstone, rev bumped);
  - tombstones are invisible to list/get and can't be updated;
  - a non-syncable table (`products`) is unaffected (no rev, hard delete);
  - the internal `_sync_seq` table is not REST-exposed.
Booted by run_with_server (which seeds `items` and passes CEL_DATA_DB so we can peek
the file directly to prove the tombstone persists and _sync_seq tracks).
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


def peek(sql):
    """Read straight from the app's data.db (the harness exports CEL_DATA_DB)."""
    db = os.environ.get("CEL_DATA_DB")
    if not db: return None
    con = sqlite3.connect(db)
    try: return con.execute(sql).fetchone()
    finally: con.close()


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<48} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar sync-substrate harness -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    # ---- create stamps rev=1, deleted=0 (client spoof ignored) ----
    s, b = req("POST", "/api/items", {"name": "a", "rev": 99, "deleted": 1}, token=admin)
    row = (b or {}).get("row", {})
    chk("create -> 201", s == 201, f"status={s}")
    chk("rev stamped = 1 (client rev=99 ignored)", row.get("rev") == 1, str(row.get("rev")))
    chk("deleted = 0 (client deleted=1 ignored)", row.get("deleted") == 0, str(row.get("deleted")))
    id_a = row.get("id")

    # ---- second create -> rev=2 ----
    s, b = req("POST", "/api/items", {"name": "b"}, token=admin)
    id_b = (b or {}).get("row", {}).get("id")
    chk("second create -> rev=2", (b or {}).get("row", {}).get("rev") == 2, str(b))

    # ---- update bumps rev (spoofed rev ignored) ----
    s, b = req("PATCH", f"/api/items/{id_a}", {"name": "a2", "rev": 50}, token=admin)
    row = (b or {}).get("row", {})
    chk("update -> 200", s == 200, f"status={s}")
    chk("update bumps rev to 3 (spoof ignored)", row.get("rev") == 3, str(row.get("rev")))
    chk("update applied the name", row.get("name") == "a2", str(row.get("name")))

    # ---- delete is a soft-delete: row hidden but physically present ----
    s, b = req("DELETE", f"/api/items/{id_a}", token=admin)
    chk("delete -> 200", s == 200, f"status={s}")
    s, _ = req("GET", f"/api/items/{id_a}", token=admin)
    chk("get tombstone -> 404", s == 404, f"status={s}")
    s, b = req("GET", "/api/items?count=exact", token=admin)
    names = sorted(r["name"] for r in (b or {}).get("rows", []))
    chk("list hides tombstone (only 'b' live)", names == ["b"], str(names))
    s, _ = req("PATCH", f"/api/items/{id_a}", {"name": "zombie"}, token=admin)
    chk("update tombstone -> 404", s == 404, f"status={s}")
    s, _ = req("DELETE", f"/api/items/{id_a}", token=admin)
    chk("re-delete tombstone -> 404", s == 404, f"status={s}")

    # ---- direct file peek: the tombstone really persists, rev tracked ----
    tomb = peek(f"SELECT deleted, rev FROM items WHERE id = '{id_a}'")
    chk("tombstone physically present (deleted=1)", tomb is not None and tomb[0] == 1, str(tomb))
    seq = peek("SELECT seq FROM _sync_seq WHERE id = 1")
    chk("_sync_seq advanced to 4 (1 create+create+update+delete)", seq and seq[0] == 4, str(seq))

    # ---- non-syncable table (products) is unaffected: no rev, real delete ----
    s, b = req("POST", "/api/products", {"name": "z", "sku": "SYNC-Z", "price": 1}, token=admin)
    prow = (b or {}).get("row", {})
    pid = prow.get("id")
    chk("product create has no rev field (not syncable)", "rev" not in prow, str(list(prow.keys())))
    s, _ = req("DELETE", f"/api/products/{pid}", token=admin)
    chk("product delete -> 200", s == 200, f"status={s}")
    gone = peek(f"SELECT count(*) FROM products WHERE id = '{pid}'")
    chk("product really removed (hard delete)", gone is not None and gone[0] == 0, str(gone))

    # ---- the internal _sync_seq table is not a REST resource ----
    s, _ = req("GET", "/api/_sync_seq", token=admin)
    chk("_sync_seq not REST-exposed -> 404", s == 404, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
