#!/usr/bin/env python3
"""cellar session-cache end-to-end (#55): prove the cache actually serves auth.

With CEL_SESSION_CACHE_TTL>0, a token resolved once is cached. We then DELETE its
session row directly in the DB — without a cache, the next authed request would
401; with the cache it still succeeds (within TTL). That a *deleted* session keeps
working for the TTL window is exactly the staleness the design documents, and is
unambiguous proof the cache is on the request path. Run via the harness with the
cache enabled. Reads CEL_DB_* from the environment for the psql step.
"""
import http.client, json, os, subprocess, sys
from urllib.parse import urlparse

HOST = PORT = None
USER = ("editor@cellar.dev", "editor-pw")


def req(method, path, body=None, token=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    headers = {}
    if body is not None: headers["Content-Type"] = "application/json"
    if token: headers["Authorization"] = "Bearer " + token
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=headers)
    resp = c.getresponse(); resp.read(); c.close()
    return resp.status


def psql(sql):
    subprocess.run(
        ["psql", "-h", os.environ.get("CEL_DB_HOST", "localhost"),
         "-U", os.environ.get("CEL_DB_USER", "postgres"),
         "-d", os.environ.get("CEL_DB_NAME", "cellar"), "-c", sql],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<34} {detail}")
        ok += bool(cond); fail += (not cond)

    s, b = None, None
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    c.request("POST", "/auth/login", body=json.dumps({"email": USER[0], "password": USER[1]}),
              headers={"Content-Type": "application/json"})
    r = c.getresponse(); token = (json.loads(r.read()) or {}).get("token"); c.close()
    chk("login", bool(token))

    chk("authed request 200", req("GET", "/schema", token=token) == 200)   # warms the cache

    # a bogus token must 401 — confirms 401 is reachable (the cache isn't blanket-allowing)
    chk("bogus token 401", req("GET", "/schema", token="deadbeef") == 401)

    # delete the session row out from under the token; the cache should still serve
    # it. Tokens are stored hashed at rest, so delete by sha256(token).
    import hashlib
    thash = hashlib.sha256(token.encode()).hexdigest()
    psql(f"DELETE FROM cel_sessions WHERE token = '{thash}'")
    chk("deleted-session still 200 (cache)", req("GET", "/schema", token=token) == 200)

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
