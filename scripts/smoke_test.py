#!/usr/bin/env python3
"""cellar smoke test: framed PING/ECHO/INFO + full auth flow (login/verify/logout)."""
import asyncio, struct, sys, json, os
import websockets

HEADER = struct.Struct("!BBHI")  # opcode, flags, message_id, payload_len  (big-endian)
FLAG_RESPONSE = 0x80
FLAG_ERROR = 0x40

# opcodes
OP_PING, OP_ECHO, OP_INFO = 0x01, 0x02, 0x03
OP_LOGIN, OP_LOGOUT, OP_VERIFY = 0x10, 0x11, 0x12
OP_DB_SCHEMA = 0xD7
OP_DB_LIST, OP_DB_GET = 0xD0, 0xD1
OP_DB_CREATE, OP_DB_UPDATE, OP_DB_DELETE = 0xD2, 0xD3, 0xD4

def frame(opcode, msg_id, payload=b""):
    if isinstance(payload, (dict, list)):
        payload = json.dumps(payload).encode()
    return HEADER.pack(opcode, 0, msg_id, len(payload)) + payload

def parse(data):
    op, flags, mid, plen = HEADER.unpack(data[:8])
    return op, flags, mid, data[8:8 + plen]

class Runner:
    def __init__(self): self.failures = 0; self.mid = 0
    def next_id(self): self.mid += 1; return self.mid
    async def call(self, ws, opcode, payload=b""):
        mid = self.next_id()
        await ws.send(frame(opcode, mid, payload))
        return parse(await ws.recv())
    def check(self, name, cond, detail=""):
        print(f"{name:<22} {'OK' if cond else 'FAIL'}  {detail}")
        self.failures += not cond

async def main(url, email, password):
    r = Runner()
    async with websockets.connect(url, subprotocols=["binary"]) as ws:
        op, fl, mid, pl = await r.call(ws, OP_PING)
        r.check("PING -> PONG", op == OP_PING | FLAG_RESPONSE and pl == b"PONG")

        op, fl, mid, pl = await r.call(ws, OP_ECHO, b"hello cellar")
        r.check("ECHO", pl == b"hello cellar")

        op, fl, mid, pl = await r.call(ws, OP_INFO)
        r.check("SERVER_INFO", b"cellar" in pl)

        # bad login -> error flag + error status
        op, fl, mid, pl = await r.call(ws, OP_LOGIN, {"email": email, "password": "wrong-password"})
        body = json.loads(pl)
        r.check("LOGIN (bad creds)", (fl & FLAG_ERROR) and body.get("status") == "error", body.get("message", ""))

        # good login -> token + user
        op, fl, mid, pl = await r.call(ws, OP_LOGIN, {"email": email, "password": password})
        body = json.loads(pl)
        token = body.get("token")
        ok = not (fl & FLAG_ERROR) and body.get("status") == "ok" and isinstance(token, str) and len(token) == 64
        r.check("LOGIN (good creds)", ok, f"role={body.get('user', {}).get('role')} token={(token or '')[:12]}…")

        if token:
            op, fl, mid, pl = await r.call(ws, OP_VERIFY, {"token": token})
            body = json.loads(pl)
            r.check("VERIFY (valid)", body.get("status") == "ok" and body.get("user", {}).get("email") == email)

        # anon (no token) must be denied on the data layer
        _, fl, _, pl = await r.call(ws, OP_DB_LIST, {"table": "products"})
        r.check("WS anon denied", (fl & FLAG_ERROR) and json.loads(pl).get("status") == "error")

        T = {"token": token}   # admin token threaded into every authenticated op

        # schema catalog (requires auth)
        op, fl, mid, pl = await r.call(ws, OP_DB_SCHEMA, T)
        cat = json.loads(pl)
        tables = {t["name"]: t for t in cat.get("tables", [])}
        r.check("DB_SCHEMA tables", {"products", "categories"} <= set(tables),
                "found=" + ",".join(sorted(tables)))
        r.check("DB_SCHEMA no internals", "cel_users" not in tables and "cel_sessions" not in tables)
        prod = tables.get("products", {})
        r.check("DB_SCHEMA primary_key", prod.get("primary_key") == "id")
        cols = {c["name"]: c for c in prod.get("columns", [])}
        r.check("DB_SCHEMA types", cols.get("price", {}).get("type") == "numeric"
                and cols.get("in_stock", {}).get("type") == "bigint"  # SQLite ints are 64-bit
                and cols.get("is_active", {}).get("type") == "bool")
        fk = cols.get("category_id", {}).get("references")
        r.check("DB_SCHEMA foreign_key", fk == {"table": "categories", "column": "id"}, str(fk))

        # ---- read path (DB_LIST / DB_GET) ----
        _, _, _, pl = await r.call(ws, OP_DB_LIST, {"table": "categories", **T})
        body = json.loads(pl)
        r.check("DB_LIST categories", body.get("status") == "ok" and body.get("count") == 2,
                f"count={body.get('count')}")

        _, _, _, pl = await r.call(ws, OP_DB_LIST, {
            "table": "products", "select": ["name", "price", "in_stock"],
            "where": {"in_stock": {"gt": 200}}, "order": ["-price"], "limit": 10, **T})
        body = json.loads(pl)
        rows = body.get("rows", [])
        ok = (body.get("count") == 2
              and [x["name"] for x in rows] == ["Cola 330ml", "Water 500ml"]   # price desc
              and isinstance(rows[0]["price"], (int, float))
              and isinstance(rows[0]["in_stock"], int))
        r.check("DB_LIST where/order/types", ok, str([x["name"] for x in rows]))

        # grab an id, then DB_GET it
        _, _, _, pl = await r.call(ws, OP_DB_LIST, {"table": "products", "select": ["id", "name"], "limit": 1, **T})
        pid = json.loads(pl)["rows"][0]["id"]
        _, _, _, pl = await r.call(ws, OP_DB_GET, {"table": "products", "id": pid, **T})
        body = json.loads(pl)
        r.check("DB_GET by id", body.get("status") == "ok" and body.get("row", {}).get("id") == pid)

        _, fl, _, pl = await r.call(ws, OP_DB_GET, {"table": "products",
                                                    "id": "00000000-0000-0000-0000-000000000000", **T})
        r.check("DB_GET not found", (fl & FLAG_ERROR) and json.loads(pl).get("status") == "error")

        # ---- security: internal tables hidden, identifiers validated, values bound ----
        _, fl, _, pl = await r.call(ws, OP_DB_LIST, {"table": "cel_users", **T})
        r.check("DB_LIST internal hidden", json.loads(pl).get("message") == "unknown table")

        _, fl, _, pl = await r.call(ws, OP_DB_LIST, {"table": "products",
                                                     "order": ["name; DROP TABLE products"], **T})
        r.check("injection via identifier blocked",
                (fl & FLAG_ERROR) and "unknown column" in json.loads(pl).get("message", ""))

        # malicious VALUE must be treated as a literal bind (no injection, no error)
        await r.call(ws, OP_DB_LIST, {"table": "products",
                                      "where": {"name": {"eq": "'; DROP TABLE products; --"}}, **T})
        _, _, _, pl = await r.call(ws, OP_DB_LIST, {"table": "products", "select": ["id"], **T})
        r.check("injection via value neutralized", json.loads(pl).get("count") == 3,
                "products table intact")

        # ---- write path over WebSocket opcodes (admin token; self-cleaning) ----
        _, fl, _, pl = await r.call(ws, OP_DB_CREATE, {
            "table": "products",
            "values": {"name": "WS Widget", "sku": "WS-CRUD-1", "price": 3.0, "in_stock": 4}, **T})
        body = json.loads(pl)
        wid = body.get("row", {}).get("id")
        r.check("WS create", not (fl & FLAG_ERROR) and body.get("status") == "ok" and wid)

        _, _, _, pl = await r.call(ws, OP_DB_UPDATE, {
            "table": "products", "id": wid, "values": {"in_stock": 99}, **T})
        r.check("WS update", json.loads(pl).get("row", {}).get("in_stock") == 99)

        _, fl, _, pl = await r.call(ws, OP_DB_CREATE, {
            "table": "products", "values": {"name": "Dup", "sku": "WS-CRUD-1", "price": 1}, **T})
        r.check("WS create dup -> error", (fl & FLAG_ERROR) and json.loads(pl).get("status") == "error")

        _, _, _, pl = await r.call(ws, OP_DB_DELETE, {"table": "products", "id": wid, **T})
        r.check("WS delete", json.loads(pl).get("status") == "ok")
        _, fl, _, pl = await r.call(ws, OP_DB_GET, {"table": "products", "id": wid, **T})
        r.check("WS get after delete -> error", (fl & FLAG_ERROR))

        # ---- session lifecycle: logout invalidates the token (run last) ----
        if token:
            op, fl, mid, pl = await r.call(ws, OP_LOGOUT, {"token": token})
            r.check("LOGOUT", json.loads(pl).get("status") == "ok")
            op, fl, mid, pl = await r.call(ws, OP_VERIFY, {"token": token})
            r.check("VERIFY (after logout)", (fl & FLAG_ERROR) and json.loads(pl).get("status") == "error")

    print("\nRESULT:", "ALL PASS" if r.failures == 0 else f"{r.failures} FAILED")
    sys.exit(1 if r.failures else 0)

if __name__ == "__main__":
    url   = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/"
    email = os.environ.get("CEL_TEST_EMAIL", "admin@cellar.dev")
    pw    = os.environ.get("CEL_TEST_PASSWORD", "s3cret-admin")
    asyncio.run(main(url, email, pw))
