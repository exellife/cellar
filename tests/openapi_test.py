#!/usr/bin/env python3
"""pgforge OpenAPI generation: GET /openapi.json returns a valid OpenAPI 3.0
document auto-generated from the live schema catalog (auth-gated like /schema),
with a path + component schema per table. Run as: openapi_test.py ws://host:port/
"""
import http.client, json, sys
from urllib.parse import urlparse

HOST = PORT = None
ADMIN = ("admin@pgforge.dev", "s3cret-admin")


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


def main():
    ok = 0; fail = 0
    def chk(n, cond, d=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {n:<42} {d}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge OpenAPI harness -> {HOST}:{PORT} ==")

    # auth-gated, like /schema
    s, _ = req("GET", "/openapi.json")
    chk("unauthenticated -> 401", s == 401, str(s))

    s, b = req("POST", "/auth/login", {"email": ADMIN[0], "password": ADMIN[1]})
    token = (b or {}).get("token")
    chk("admin login", bool(token))

    s, doc = req("GET", "/openapi.json", token=token)
    chk("authenticated -> 200", s == 200, str(s))
    doc = doc or {}
    chk("openapi 3.0.x", str(doc.get("openapi", "")).startswith("3.0"), str(doc.get("openapi")))
    chk("info.title", doc.get("info", {}).get("title") == "pgforge API")

    paths = doc.get("paths", {})
    chk("has /api/products",      "/api/products" in paths)
    chk("has /api/products/{id}", "/api/products/{id}" in paths)
    chk("products: GET + POST",   {"get", "post"} <= set(paths.get("/api/products", {}).keys()))
    chk("products/{id}: get/patch/delete",
        {"get", "patch", "delete"} <= set(paths.get("/api/products/{id}", {}).keys()))
    chk("has /api/categories",    "/api/categories" in paths)
    chk("has /auth/login",        "/auth/login" in paths)

    schemas = doc.get("components", {}).get("schemas", {})
    prod = schemas.get("products", {})
    props = prod.get("properties", {})
    chk("products schema has columns",
        all(c in props for c in ("id", "name", "sku", "price")), str(sorted(props)[:6]))
    chk("PK 'id' is readOnly", props.get("id", {}).get("readOnly") is True)
    chk("defaulted col not readOnly", props.get("price", {}).get("readOnly") is None)
    chk("'name' required (NOT NULL, no default)", "name" in prod.get("required", []))
    chk("'id' NOT required (server-set)", "id" not in prod.get("required", []))
    chk("uuid column typed string/uuid",
        props.get("id", {}).get("type") == "string" and props.get("id", {}).get("format") == "uuid")
    chk("bearer security scheme",
        "bearerAuth" in doc.get("components", {}).get("securitySchemes", {}))

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
