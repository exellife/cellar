#!/usr/bin/env python3
"""cellar self-service registration test (#51), over REST.

Boots against the taxi example policy (rider/driver are self-registerable; admin
is not). Proves: a self-registerable role can sign up and is auto-logged-in; the
new token works; duplicate email -> 409; superuser/non-self-registerable/unknown
roles -> 403; weak/short password -> 400; the default role is used when omitted.

Run as: register_test.py ws://127.0.0.1:<port>/   (host/port parsed from argv).
"""
import http.client, json, sys, os, binascii
from urllib.parse import urlparse

HOST = PORT = None


class R:
    def __init__(self): self.ok = 0; self.fail = 0
    def check(self, name, cond, detail=""):
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<38} {detail}")
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


def uniq(prefix):
    return f"{prefix}-{binascii.hexlify(os.urandom(5)).decode()}@taxi.test"


def main():
    r = R()
    print(f"== cellar register harness -> {HOST}:{PORT} ==")

    # 1. happy path: register a rider (self-registerable), auto-login.
    rider = uniq("rider")
    s, b = req("POST", "/auth/register", {"email": rider, "password": "ride-along-1", "role": "rider"})
    r.check("rider register 201", s == 201, str(s))
    token = (b or {}).get("token")
    r.check("register returns token", bool(token))
    r.check("register returns role", (b or {}).get("user", {}).get("role") == "rider")

    # 2. the issued token is a working session.
    s, b = req("GET", "/schema", token=token)
    r.check("new token authenticates", s == 200, str(s))

    # 3. duplicate email -> 409.
    s, b = req("POST", "/auth/register", {"email": rider, "password": "ride-along-1", "role": "rider"})
    r.check("duplicate email 409", s == 409, str(s))

    # 4. role omitted -> uses the default self-registerable role (rider, first in config).
    s, b = req("POST", "/auth/register", {"email": uniq("def"), "password": "ride-along-1"})
    r.check("default role 201", s == 201, str(s))
    r.check("default role is rider", (b or {}).get("user", {}).get("role") == "rider")

    # 5. driver is also self-registerable.
    s, b = req("POST", "/auth/register", {"email": uniq("drv"), "password": "drive-fast-1", "role": "driver"})
    r.check("driver register 201", s == 201, str(s))

    # 6. boundary: a superuser role can never be obtained via signup.
    s, b = req("POST", "/auth/register", {"email": uniq("hax"), "password": "want-admin-1", "role": "admin"})
    r.check("admin signup blocked 403", s == 403, str(s))

    # 7. a role that isn't flagged self_register -> 403.
    s, b = req("POST", "/auth/register", {"email": uniq("ed"), "password": "editor-pw-1", "role": "editor"})
    r.check("non-self-reg role 403", s == 403, str(s))

    # 8. short password rejected -> 400.
    s, b = req("POST", "/auth/register", {"email": uniq("weak"), "password": "short", "role": "rider"})
    r.check("short password 400", s == 400, str(s))

    # --- admin-provisioned accounts (#51b): a superuser creates users via API ---
    s, b = req("POST", "/auth/login", {"email": "admin@cellar.dev", "password": "s3cret-admin"})
    admin = (b or {}).get("token")
    r.check("admin login", bool(admin))

    # admin provisions a driver account (single-tenant: no tenant_id needed).
    drv = uniq("prov-drv")
    s, b = req("POST", "/auth/users", {"email": drv, "password": "drive-fast-1", "role": "driver"}, token=admin)
    r.check("admin creates driver 201", s == 201, str(s))
    r.check("created role is driver", (b or {}).get("user", {}).get("role") == "driver")
    r.check("created user has no token", "token" not in (b or {}))   # not auto-logged-in

    # the provisioned user can log in themselves.
    s, b = req("POST", "/auth/login", {"email": drv, "password": "drive-fast-1"})
    r.check("provisioned user can log in", s == 200 and bool((b or {}).get("token")), str(s))

    # boundary: a non-superuser (the rider) cannot provision accounts.
    s, b = req("POST", "/auth/users", {"email": uniq("x"), "password": "whatever-1", "role": "rider"}, token=token)
    r.check("non-superuser create 403", s == 403, str(s))

    # boundary: nobody can mint a platform_admin via the API.
    s, b = req("POST", "/auth/users", {"email": uniq("pa"), "password": "global-boss-1", "role": "platform_admin"}, token=admin)
    r.check("platform_admin create 403", s == 403, str(s))

    # unauthenticated -> 401.
    s, b = req("POST", "/auth/users", {"email": uniq("anon"), "password": "no-token-1", "role": "driver"})
    r.check("unauthenticated create 401", s == 401, str(s))

    # missing role -> 400.
    s, b = req("POST", "/auth/users", {"email": uniq("norole"), "password": "no-role-1"}, token=admin)
    r.check("missing role 400", s == 400, str(s))

    # duplicate email -> 409.
    s, b = req("POST", "/auth/users", {"email": drv, "password": "drive-fast-1", "role": "driver"}, token=admin)
    r.check("admin create duplicate 409", s == 409, str(s))

    print(f"\n{'PASS' if r.fail == 0 else 'FAIL'}  ({r.ok} ok, {r.fail} failed)")
    return 1 if r.fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
