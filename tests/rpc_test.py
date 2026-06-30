#!/usr/bin/env python3
"""cellar rpc-as-Lua-hook end-to-end (Phase 2). POST /rpc/<fn> dispatches to the
bundle's hooks.lua rpc(name, args, who): scalar results, the request identity in
`who`, args from the body, db reads/writes via cellar.query/exec, and faults /
unknown handlers surfacing as errors (not crashes). Booted by run_with_server,
which seeds the demo catalog + a hooks.lua with these handlers.
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


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<40} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar rpc-hook harness -> {HOST}:{PORT} ==")
    admin = login(ADMIN)
    chk("login admin", bool(admin))

    # ---- scalar result ----
    s, b = req("POST", "/rpc/ping", token=admin)
    chk("rpc ping -> 200 {result.pong}", s == 200 and (b or {}).get("result", {}).get("pong") is True,
        f"status={s} body={b}")

    # ---- the request identity reaches the hook as `who` ----
    s, b = req("POST", "/rpc/whoami", token=admin)
    res = (b or {}).get("result", {})
    chk("rpc whoami -> caller role/auth", s == 200 and res.get("role") == "admin" and res.get("auth") is True,
        str(res))

    # ---- args from the body ----
    s, b = req("POST", "/rpc/echo", {"msg": "hello hooks"}, token=admin)
    chk("rpc echo -> args echoed", s == 200 and (b or {}).get("result", {}).get("got") == "hello hooks",
        str(b))

    # ---- db read via cellar.query (3 seeded products) ----
    s, b = req("POST", "/rpc/count_products", token=admin)
    chk("rpc count_products via cellar.query", s == 200 and (b or {}).get("result", {}).get("n") == 3,
        str(b))

    # ---- db write via cellar.exec, then re-count -> 4 ----
    s, b = req("POST", "/rpc/add_product",
               {"name": "Hook Widget", "sku": "HOOK-1", "price": 9.5}, token=admin)
    chk("rpc add_product via cellar.exec", s == 200 and (b or {}).get("result", {}).get("ok") is True, str(b))
    s, b = req("POST", "/rpc/count_products", token=admin)
    chk("count reflects the insert -> 4", s == 200 and (b or {}).get("result", {}).get("n") == 4, str(b))

    # ---- a unique-violation inside the hook surfaces as an error, not a crash ----
    s, b = req("POST", "/rpc/add_product",
               {"name": "Dup", "sku": "HOOK-1", "price": 1}, token=admin)
    chk("duplicate sku in hook -> 400", s == 400, f"status={s}")

    # ---- cellar.create_user primitive: a hook mints a login account ----
    s, b = req("POST", "/rpc/create_user",
               {"email": "hired@cellar.dev", "password": "hired-pw1", "role": "editor"}, token=admin)
    res = (b or {}).get("result", {})
    chk("rpc create_user -> new id", s == 200 and bool(res.get("id")) and not res.get("error"), str(b))
    chk("minted account can log in", bool(login(("hired@cellar.dev", "hired-pw1"))), "")
    # duplicate email surfaces as an error, not a crash
    s, b = req("POST", "/rpc/create_user",
               {"email": "hired@cellar.dev", "password": "another-pw", "role": "editor"}, token=admin)
    chk("create_user duplicate -> error", s == 200 and "already registered" in ((b or {}).get("result", {}).get("error") or ""),
        str(b))
    # the escalation boundary holds: NO superuser role can be minted from a hook —
    # not platform_admin, and not 'admin' (which the engine treats as a superuser).
    s, b = req("POST", "/rpc/create_user",
               {"email": "evil@cellar.dev", "password": "evil-pw12", "role": "platform_admin"}, token=admin)
    chk("create_user platform_admin refused", s == 200 and "superuser" in ((b or {}).get("result", {}).get("error") or "").lower(),
        str(b))
    s, b = req("POST", "/rpc/create_user",
               {"email": "wannabe-admin@cellar.dev", "password": "sneaky-pw1", "role": "admin"}, token=admin)
    chk("create_user 'admin' (superuser) refused", s == 200 and "superuser" in ((b or {}).get("result", {}).get("error") or "").lower(),
        str(b))
    # and weak passwords are rejected (the primitive isn't a weaker creation path)
    s, b = req("POST", "/rpc/create_user",
               {"email": "weak@cellar.dev", "password": "short", "role": "editor"}, token=admin)
    chk("create_user weak password refused", s == 200 and "too short" in ((b or {}).get("result", {}).get("error") or ""),
        str(b))

    # ---- cellar.set_password primitive: a hook resets an EXISTING account's password ----
    # reset the account we minted above, then prove new works + old is rejected + sessions revoked
    s, b = req("POST", "/rpc/set_password",
               {"email": "hired@cellar.dev", "new_password": "reset-pw-99"}, token=admin)
    chk("rpc set_password -> ok", s == 200 and (b or {}).get("result", {}).get("ok") is True, str(b))
    chk("login with the NEW password works", bool(login(("hired@cellar.dev", "reset-pw-99"))), "")
    chk("login with the OLD password rejected", not login(("hired@cellar.dev", "hired-pw1")), "")
    # the escalation boundary holds: a hook CANNOT reset a superuser's password (admin takeover)
    s, b = req("POST", "/rpc/set_password",
               {"email": "admin@cellar.dev", "new_password": "pwned-admin1"}, token=admin)
    chk("set_password superuser refused", s == 200 and "superuser" in ((b or {}).get("result", {}).get("error") or "").lower(),
        str(b))
    chk("admin password unchanged (still logs in)", bool(login(ADMIN)), "")
    # unknown email → a clear error, not a crash (and no info leak beyond "no account")
    s, b = req("POST", "/rpc/set_password",
               {"email": "ghost@cellar.dev", "new_password": "whatever-12"}, token=admin)
    chk("set_password unknown email -> error", s == 200 and "no account" in ((b or {}).get("result", {}).get("error") or "").lower(),
        str(b))
    # weak passwords rejected (not a weaker path)
    s, b = req("POST", "/rpc/set_password",
               {"email": "hired@cellar.dev", "new_password": "short"}, token=admin)
    chk("set_password weak password refused", s == 200 and "too short" in ((b or {}).get("result", {}).get("error") or ""),
        str(b))

    # ---- device tokens are OFF unless the app opts in (no _session here) ----
    s, b = req("POST", "/auth/device", {"label": "x"}, token=admin)
    chk("device tokens off by default -> 404", s == 404, f"status={s}")

    # ---- rpc authorization (fail-closed _rpc whitelist) ----
    # admin is a superuser → reaches every LISTED rpc above. Now the role paths for a
    # non-superuser, and the deny paths:
    viewer = login(("viewer@cellar.dev", "viewer-pw"))
    chk("login viewer", bool(viewer))
    s, b = req("POST", "/rpc/ping", token=viewer)   # ping whitelisted for viewer
    chk("viewer allowed for whitelisted rpc -> 200", s == 200 and (b or {}).get("result", {}).get("pong") is True,
        f"status={s}")
    s, b = req("POST", "/rpc/echo", {"msg": "x"}, token=viewer)   # echo is editor-only
    chk("viewer denied for role -> 403", s == 403, f"status={s}")
    s, b = req("POST", "/rpc/ping")   # anon, ping not anon-whitelisted
    chk("anon rpc -> 401", s == 401, f"status={s}")

    # ---- unknown/unlisted + faulting handler ----
    # an UNLISTED fn is denied at the authz gate (uniform with unauthorized → no
    # existence oracle): 403, not the old 400 "unknown rpc".
    s, b = req("POST", "/rpc/nope", token=admin)
    chk("unlisted rpc -> 403 (not an existence oracle)", s == 403, f"status={s}")
    s, b = req("POST", "/rpc/boom", token=admin)   # boom IS listed → reaches the fault
    chk("faulting rpc -> 400 (no crash)", s == 400, f"status={s}")

    # server still healthy after the fault
    s, b = req("POST", "/rpc/ping", token=admin)
    chk("server healthy after fault", s == 200 and (b or {}).get("result", {}).get("pong") is True, "")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
