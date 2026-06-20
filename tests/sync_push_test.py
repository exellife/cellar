#!/usr/bin/env python3
"""cellar sync_push end-to-end (Slice 2). Booted with the resolve() bundle
(sync_push.lua) so both LWW and the override are covered.

POST /sync/push applies a batch of client mutations all-or-nothing:
  - client-id creates (offline-made rows), clean updates, soft-deletes;
  - conflict (stale base_rev) resolves LWW by default; the resolve() hook can keep
    the server's row (a client marks that by prefixing the name with 'KEEP');
  - idempotent delete of a missing row;
  - a bad mutation rolls back the whole batch;
  - a push -> pull round-trip; 401 unauthenticated.
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


def push(token, muts):
    return req("POST", "/sync/push", {"mutations": muts}, token=token)


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<50} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar sync_push harness -> {HOST}:{PORT} ==")
    tok = login(ADMIN)
    chk("login admin", bool(tok))

    # ---- client-id creates (offline-made rows) ----
    s, b = push(tok, [
        {"op": "put", "table": "items", "id": "id-a", "values": {"name": "alpha"}},
        {"op": "put", "table": "items", "id": "id-b", "values": {"name": "beta"}},
    ])
    chk("push -> 200", s == 200, f"status={s}")
    res = {r["id"]: r for r in b["results"]}
    chk("create id-a applied rev=1", res["id-a"]["status"] == "applied" and res["id-a"]["rev"] == 1, str(res.get("id-a")))
    chk("create id-b applied rev=2", res["id-b"]["rev"] == 2, str(res.get("id-b")))
    chk("cursor=2", b["cursor"] == 2, str(b["cursor"]))

    # ---- pull round-trip sees them with the client ids ----
    s, b = req("POST", "/sync/pull", {"since": 0}, token=tok)
    names = {r["id"]: r["name"] for r in b["changes"].get("items", [])}
    chk("pull reflects pushed rows", names.get("id-a") == "alpha" and names.get("id-b") == "beta", str(names))

    # ---- clean update (base_rev matches current) ----
    s, b = push(tok, [{"op": "put", "table": "items", "id": "id-a", "base_rev": 1, "values": {"name": "alpha2"}}])
    chk("clean update applied, rev=3", b["results"][0]["status"] == "applied" and b["results"][0]["rev"] == 3, str(b["results"][0]))

    # ---- conflict, LWW default (normal name -> incoming wins) ----
    s, b = push(tok, [{"op": "put", "table": "items", "id": "id-a", "base_rev": 99, "values": {"name": "alpha3"}}])
    r = b["results"][0]
    chk("conflict LWW -> winner incoming, rev=4", r["status"] == "conflict" and r["winner"] == "incoming" and r["rev"] == 4, str(r))

    # ---- conflict, resolve() override (KEEP* name -> server wins, NOT applied) ----
    s, b = push(tok, [{"op": "put", "table": "items", "id": "id-a", "base_rev": 99, "values": {"name": "KEEP-this"}}])
    r = b["results"][0]
    chk("conflict resolve -> winner server", r["status"] == "conflict" and r["winner"] == "server", str(r))
    s, g = req("GET", "/api/items/id-a", token=tok)
    chk("server row unchanged (name=alpha3, rev=4)", g["row"]["name"] == "alpha3" and g["row"]["rev"] == 4, str(g.get("row")))

    # ---- delete (clean) + idempotent delete of a missing row ----
    s, b = push(tok, [
        {"op": "del", "table": "items", "id": "id-b", "base_rev": 2},
        {"op": "del", "table": "items", "id": "id-missing"},
    ])
    rd = {r["id"]: r["status"] for r in b["results"]}
    chk("delete id-b applied", rd.get("id-b") == "applied", str(rd))
    chk("delete missing -> applied (idempotent)", rd.get("id-missing") == "applied", str(rd))
    s, _ = req("GET", "/api/items/id-b", token=tok)
    chk("deleted row hidden from get -> 404", s == 404, f"status={s}")

    # ---- batch all-or-nothing: one bad mutation rolls back the whole batch ----
    s, b = push(tok, [
        {"op": "put", "table": "items", "id": "id-c", "values": {"name": "gamma"}},
        {"op": "put", "table": "nope", "id": "x", "values": {}},   # unknown table -> hard error
    ])
    chk("bad batch -> 400", s == 400, f"status={s}")
    s, _ = req("GET", "/api/items/id-c", token=tok)
    chk("good mutation rolled back with the batch (id-c 404)", s == 404, f"status={s}")

    # ---- unauthenticated ----
    s, _ = req("POST", "/sync/push", {"mutations": []})
    chk("unauthenticated -> 401", s == 401, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
