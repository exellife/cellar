#!/usr/bin/env python3
"""pgforge REST front-door test (http.client, no external deps).

Covers schema/list/get/CRUD over HTTP, the engine's security properties
(internal tables hidden, identifier validation, value binding), and the policy
engine (anon -> 401, role-gated writes/deletes).
"""
import http.client, json, sys
from urllib.parse import urlparse, quote

ADMIN   = ("admin@pgforge.dev",   "s3cret-admin")
EDITOR  = ("editor@pgforge.dev",  "editor-pw")
EDITOR2 = ("editor2@pgforge.dev", "editor2-pw")
VIEWER  = ("viewer@pgforge.dev",  "viewer-pw")

HOST = PORT = None


class R:
    def __init__(self): self.ok = 0; self.fail = 0
    def check(self, name, cond, detail=""):
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<34} {detail}")
        self.ok += bool(cond); self.fail += (not cond)
        return cond


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
    r = R()
    print(f"== pgforge REST harness -> {HOST}:{PORT} ==")
    admin, editor, viewer = login(ADMIN), login(EDITOR), login(VIEWER)
    editor2 = login(EDITOR2)
    r.check("login (admin/editor/viewer/editor2)", all([admin, editor, viewer, editor2]))

    # ---- policy enforcement ----
    s, _ = req("GET", "/api/products")
    r.check("anon list -> 401", s == 401)
    s, _ = req("GET", "/schema")
    r.check("anon schema -> 401", s == 401)
    s, _ = req("GET", "/api/products", token="deadbeefbadtoken")
    r.check("bad token -> 401", s == 401)
    s, _ = req("GET", "/api/products", token=viewer)
    r.check("viewer list -> 200", s == 200)
    s, _ = req("POST", "/api/products", {"name": "Z", "sku": "POL-1", "price": 1}, token=viewer)
    r.check("viewer create -> 403", s == 403)

    # ---- schema / reads (admin) ----
    s, b = req("GET", "/schema", token=admin)
    tables = {t["name"] for t in (b or {}).get("tables", [])}
    r.check("GET /schema", s == 200 and {"products", "categories"} <= tables)
    r.check("schema hides internals", "pgf_users" not in tables)

    s, b = req("GET", "/api/categories", token=admin)
    r.check("GET /api/categories", s == 200 and b.get("count") == 2)

    s, b = req("GET", "/api/products?in_stock=gt.200&order=-price&select=name,price", token=admin)
    rows = (b or {}).get("rows", [])
    r.check("list filter/order/select", s == 200 and b.get("count") == 2
            and [x["name"] for x in rows] == ["Cola 330ml", "Water 500ml"]
            and set(rows[0].keys()) == {"name", "price"})

    s, b = req("GET", "/api/products?select=id&limit=1", token=admin)
    pid = b["rows"][0]["id"]
    s, b = req("GET", f"/api/products/{pid}", token=admin)
    r.check("GET /api/products/<id>", s == 200 and b["row"]["id"] == pid)

    s, _ = req("GET", "/api/pgf_users", token=admin)
    r.check("internal table -> 404", s == 404)

    s, b = req("GET", "/api/products?order=" + quote("name; DROP TABLE products"), token=admin)
    r.check("identifier injection -> 400", s == 400 and "unknown column" in (b or {}).get("message", ""))
    req("GET", "/api/products?name=" + quote("'; DROP TABLE products; --"), token=admin)
    s, b = req("GET", "/api/products?select=id", token=admin)
    r.check("value injection neutralized", s == 200 and b.get("count") == 3, "table intact")

    # ---- write lifecycle (editor creates/updates; admin deletes) ----
    s, b = req("POST", "/api/products",
               {"name": "REST Widget", "sku": "RST-CRUD-1", "price": 4.50, "in_stock": 9}, token=editor)
    nid = (b or {}).get("row", {}).get("id")
    r.check("editor POST create -> 201", s == 201 and b["row"]["name"] == "REST Widget")
    s, b = req("PATCH", f"/api/products/{nid}", {"price": 7.25, "in_stock": 2}, token=editor)
    r.check("editor PATCH update", s == 200 and b["row"]["price"] == 7.25)
    s, b = req("POST", "/api/products", {"name": "Dup", "sku": "RST-CRUD-1", "price": 1}, token=editor)
    msg = (b or {}).get("message", "")
    r.check("duplicate unique -> 409", s == 409)
    # M-7: the DB error must NOT leak the constraint/index/table/column name or value
    # (a raw unique-violation message is a cross-tenant row-existence oracle).
    r.check("409 message is generic (no schema leak)",
            not any(w in msg.lower() for w in
                    ("sku", "constraint", "products", "rst-crud", "unique", "key", "duplicate")),
            f"msg={msg!r}")
    s, b = req("POST", "/api/products", {"sku": "RST-CRUD-2", "price": 1}, token=editor)
    msg2 = (b or {}).get("message", "")
    r.check("missing not-null -> 400", s == 400)
    r.check("400 message is generic (no column leak)",
            not any(w in msg2.lower() for w in ("name", "null", "column", "violates")),
            f"msg={msg2!r}")
    s, b = req("POST", "/api/products", {"name": "X", "nope": 1}, token=editor)
    r.check("unknown column -> 400", s == 400 and "unknown column" in (b or {}).get("message", ""))
    s, _ = req("DELETE", f"/api/products/{nid}", token=editor)
    r.check("editor DELETE -> 403", s == 403)
    s, _ = req("DELETE", f"/api/products/{nid}", token=admin)
    r.check("admin DELETE -> 200", s == 200)
    s, _ = req("GET", f"/api/products/{nid}", token=admin)
    r.check("GET after delete -> 404", s == 404)
    s, b = req("GET", "/api/products?select=id", token=admin)
    r.check("table back to 3 rows", s == 200 and b.get("count") == 3)

    # ---- row-level ownership (notes table, owner_column=owner_id) ----
    s, b = req("POST", "/api/notes",
               {"title": "E1 note", "body": "secret",
                "owner_id": "00000000-0000-0000-0000-000000000000"}, token=editor)
    note = (b or {}).get("row", {})
    noteid = note.get("id")
    r.check("create note (owner forced)", s == 201 and note.get("owner_id")
            and note["owner_id"] != "00000000-0000-0000-0000-000000000000",
            "spoofed owner ignored")

    def list_ids(tok):
        s, b = req("GET", "/api/notes?select=id", token=tok)
        return s, {x["id"] for x in (b or {}).get("rows", [])}

    s, _ = req("GET", f"/api/notes/{noteid}", token=editor)
    r.check("owner GET own note -> 200", s == 200)
    s, _ = req("GET", f"/api/notes/{noteid}", token=editor2)
    r.check("other GET note -> 404", s == 404)
    _, e1 = list_ids(editor);  r.check("owner list sees note", noteid in e1)
    _, e2 = list_ids(editor2); r.check("other list hides note", noteid not in e2)
    s, _ = req("PATCH", f"/api/notes/{noteid}", {"title": "hijack"}, token=editor2)
    r.check("other PATCH -> 404", s == 404)
    s, _ = req("DELETE", f"/api/notes/{noteid}", token=editor2)
    r.check("other DELETE -> 404", s == 404)
    s, _ = req("PATCH", f"/api/notes/{noteid}", {"owner_id": "11111111-1111-1111-1111-111111111111"}, token=editor)
    r.check("owner cannot reassign owner -> 400", s == 400)
    s, _ = req("GET", f"/api/notes/{noteid}", token=admin)
    r.check("admin GET any note -> 200 (bypass)", s == 200)
    _, ai = list_ids(admin); r.check("admin list sees note (bypass)", noteid in ai)
    s, _ = req("DELETE", f"/api/notes/{noteid}", token=admin)
    r.check("admin DELETE cleanup -> 200", s == 200)

    s, _ = req("POST", "/auth/login", {"email": ADMIN[0], "password": "wrong"})
    r.check("login bad creds -> 401", s == 401)

    print(f"\n== summary: {r.ok} ok, {r.fail} failed ==")
    sys.exit(1 if r.fail else 0)


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    main()
