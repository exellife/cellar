#!/usr/bin/env python3
"""ClerkHalls sync-integrity test — exercises cellar's offline-first sync API against
a running ClerkHalls server. Self-contained and re-runnable: logs in, creates its own
venue/hall under a unique per-run id prefix, runs the matrix, and deletes everything
it made (all rows use the "sit-<nonce>-" id prefix).

  examples/clerkhalls/run.sh                  # in one terminal
  python3 examples/clerkhalls/test_sync.py    # in another

Env: CH_URL (default http://127.0.0.1:8080), CH_HOST (default localhost),
     CH_EMAIL / CH_PW (default owner@clerkhalls.local / clerkhalls).
Exits non-zero if any check fails.
"""
import binascii, json, os, sys, urllib.request, urllib.error

# Unique per-run prefix so ids + mutation_ids never collide with a previous run
# (cellar dedups by mutation_id forever, and a put onto a prior tombstone is a
# server-wins conflict — either would make a re-run look like a failure).
NONCE = binascii.hexlify(os.urandom(3)).decode()
def sid(s): return f"sit-{NONCE}-{s}"

URL  = os.environ.get("CH_URL", "http://127.0.0.1:8080")
HOST = os.environ.get("CH_HOST", "localhost")
EMAIL, PW = os.environ.get("CH_EMAIL", "owner@clerkhalls.local"), os.environ.get("CH_PW", "clerkhalls")
HDR = {"Host": HOST, "Content-Type": "application/json"}

def call(path, body=None, method="POST"):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(URL + path, data=data, headers=HDR, method=method)
    try:
        with urllib.request.urlopen(req) as r: return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        try: return e.code, json.load(e)
        except Exception: return e.code, {}

def push(muts, device="sitA"): return call("/sync/push", {"device_id": device, "mutations": muts})
def pull(since=0, device="sitA"): return call("/sync/pull", {"since": since, "device_id": device})
def put(table, _id, values, mid=None, base_rev=None):
    m = {"mutation_id": mid or ("m-" + _id), "op": "put", "table": table, "id": _id, "values": values}
    if base_rev is not None: m["base_rev"] = base_rev
    return m
def dele(table, _id, base_rev, mid=None):
    return {"mutation_id": mid or ("d-" + _id), "op": "del", "table": table, "id": _id, "base_rev": base_rev}
def res1(b): return b["results"][0]
def find(ch, table, rid): return next((r for r in ch.get(table, []) if r["id"] == rid), None)

PASS = []
def check(name, cond, detail=""):
    PASS.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + ("" if cond else "  -> " + str(detail)))

# ---- auth ----
st, b = call("/auth/login", {"email": EMAIL, "password": PW})
if st != 200 or "token" not in b:
    print("login failed (%s) — is run.sh up?" % st); sys.exit(2)
HDR["Authorization"] = "Bearer " + b["token"]

# ---- setup: our own venue + hall (cascade test needs a hall) ----
push([put("venues", sid("venue"), {"name": "SIT Venue", "sort_order": 999}),
      put("halls", sid("hall"), {"venue_id": sid("venue"), "name": "SIT Hall"})])
HALL = sid("hall")

try:
    print("== 1. cascade soft-delete (booking -> categories/items/payments) ==")
    _, b = push([
        put("bookings", sid("b1"), {"hall_id": HALL, "session": "evening", "event_type": "wedding",
            "start_date": "2026-09-01", "end_date": "2026-09-01", "customer_name": "SIT Cascade", "status": "tentative"}),
        put("booking_item_categories", sid("c1"), {"booking_id": sid("b1"), "kind": "menu", "name": "Mains", "sort_order": 0}),
        put("booking_items", sid("i1"), {"booking_id": sid("b1"), "kind": "menu", "category_id": sid("c1"), "name": "Plov", "quantity": "100", "unit_price": 300, "sort_order": 0}),
        put("payments", sid("p1"), {"booking_id": sid("b1"), "paid_at": "2026-08-01", "stage": "deposit", "method": "cash", "amount": 5000, "amount_kgs": 5000, "currency": "KGS"}),
    ])
    brev = next(r["rev"] for r in b["results"] if r["id"] == sid("b1"))
    push([dele("bookings", sid("b1"), brev)])
    ch = pull(0)[1]["changes"]
    for t, key in [("bookings", "b1"), ("booking_item_categories", "c1"), ("booking_items", "i1"), ("payments", "p1")]:
        row = find(ch, t, sid(key))
        check(f"{t} tombstoned (deleted=1, rev bumped)", bool(row) and row.get("deleted") == 1 and row.get("rev", 0) > 0, row)

    print("== 2. idempotent retry (same mutation_id) ==")
    mid = sid("mid-1")
    rev_a = res1(push([put("venues", sid("v2"), {"name": "SIT Idem", "sort_order": 998}, mid=mid)])[1])["rev"]
    r2 = res1(push([put("venues", sid("v2"), {"name": "CHANGED", "sort_order": 998}, mid=mid)])[1])
    check("retry deduped", r2.get("deduped") == True, r2)
    check("retry same rev (no bump)", r2["rev"] == rev_a, (rev_a, r2["rev"]))
    row = find(pull(0)[1]["changes"], "venues", sid("v2"))
    check("retry did not apply changed values", row and row["name"] == "SIT Idem", row and row.get("name"))

    print("== 3. conflict resolution — LWW by updated_at ==")
    r1 = res1(push([put("venues", sid("v3"), {"name": "base", "sort_order": 997, "updated_at": "2026-01-01T00:00:00Z"})])[1])["rev"]
    c = res1(push([put("venues", sid("v3"), {"name": "B-mar", "sort_order": 997, "updated_at": "2026-03-01T00:00:00Z"}, mid=sid("vc-b"), base_rev=r1)], device="sitB")[1])
    check("B update applied", c["status"] == "applied", c)
    c = res1(push([put("venues", sid("v3"), {"name": "A-feb", "sort_order": 997, "updated_at": "2026-02-01T00:00:00Z"}, mid=sid("vc-a1"), base_rev=r1)])[1])
    check("stale-older -> conflict/server-wins", c["status"] == "conflict" and c.get("winner") == "server", c)
    check("server value kept (B-mar)", (lambda r: r and r["name"] == "B-mar")(find(pull(0)[1]["changes"], "venues", sid("v3"))), "?")
    c = res1(push([put("venues", sid("v3"), {"name": "A-apr", "sort_order": 997, "updated_at": "2026-04-01T00:00:00Z"}, mid=sid("vc-a2"), base_rev=r1)])[1])
    check("stale-newer -> conflict/incoming-wins", c["status"] == "conflict" and c.get("winner") == "incoming", c)
    check("incoming value applied (A-apr)", (lambda r: r and r["name"] == "A-apr")(find(pull(0)[1]["changes"], "venues", sid("v3"))), "?")

    print("== 4. delete with stale base_rev -> conflict but applies (by design) ==")
    rv = res1(push([put("venues", sid("v4"), {"name": "todelete", "sort_order": 996})])[1])["rev"]
    push([put("venues", sid("v4"), {"name": "bumped", "sort_order": 996}, mid=sid("v4u"), base_rev=rv)])
    c = res1(push([dele("venues", sid("v4"), rv)])[1])  # stale base_rev
    check("stale delete reported conflict/incoming", c["status"] == "conflict" and c.get("winner") == "incoming", c)
    check("row actually deleted", (lambda r: r and r.get("deleted") == 1)(find(pull(0)[1]["changes"], "venues", sid("v4"))), "?")

    print("== 5. delete of nonexistent id -> idempotent no-op ==")
    st, c = push([dele("venues", sid("nope"), 0)])
    check("nonexistent delete = applied, rev 0", st == 200 and res1(c)["status"] == "applied" and res1(c)["rev"] == 0, (st, res1(c)))

    print("== 6. device cursors + delta pull ==")
    cx = pull(0, device="sitX")[1]["cursor"]
    push([put("venues", sid("v5"), {"name": "cursor", "sort_order": 995})], device="sitY")
    px2 = pull(cx, device="sitX")[1]
    check("delta pull returns new row", bool(find(px2["changes"], "venues", sid("v5"))), list(px2["changes"]))
    check("delta cursor advanced", px2["cursor"] > cx, (cx, px2["cursor"]))
    check("delta excludes old rows (v2 absent)", find(px2["changes"], "venues", sid("v2")) is None, "leaked")

    print("== 7. tombstone hidden from REST read, present in pull ==")
    st, _ = call("/api/venues/" + sid("v4"), method="GET")
    check("GET deleted row -> 404", st == 404, st)
    check("deleted row in pull (deleted=1)", (lambda r: r and r.get("deleted") == 1)(find(pull(0)[1]["changes"], "venues", sid("v4"))), "?")

    print("== 8. batch atomicity (one bad mutation rolls back the whole batch) ==")
    st, _ = push([
        put("venues", sid("v6"), {"name": "rollback", "sort_order": 994}),
        put("booking_items", sid("bad"), {"booking_id": "does-not-exist", "kind": "menu", "name": "x", "quantity": "1", "unit_price": 1, "sort_order": 0}),
    ])
    check("bad batch rejected (non-200)", st != 200, st)
    check("valid sibling NOT persisted (rolled back)", find(pull(0)[1]["changes"], "venues", sid("v6")) is None, "persisted!")

finally:
    # cleanup: delete every live sit-* booking (cascades children) + venue (cascades halls)
    ch = pull(0)[1]["changes"]
    muts = [dele(t, r["id"], r["rev"], mid="clr-" + r["id"])
            for t in ("bookings", "venues")
            for r in ch.get(t, [])
            if r["id"].startswith("sit-") and not r.get("deleted")]
    if muts: push(muts, device="sitcleanup")

ok = sum(PASS)
print(f"\n== RESULT: {ok}/{len(PASS)} checks passed ==")
sys.exit(0 if ok == len(PASS) else 1)
