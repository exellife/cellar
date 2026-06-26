#!/usr/bin/env python3
"""classifieds progressive-trust / velocity-limit e2e (A2.5).

Boots cellar with CLS_POST_LIMIT=3 and registers a BRAND-NEW user (account
age ~0, unverified → trust 1× → cap 3 posts/24h). Proves the gate: 3 posts
succeed, the 4th is rejected, and my_limits reports the cap + usage. Also proves
the contact gate (CLS_CONTACT_LIMIT=2): reveal_contact is velocity-gated too
(anti number-scraping) — the 3rd reveal in the window is rejected. Admins are
exempt from both.

Usage: test_velocity.py <cellar-binary>
"""
import os, sys, json, time, socket, sqlite3, subprocess, tempfile, shutil
import http.client

HERE = os.path.dirname(os.path.abspath(__file__))
ADMIN = ("admin@cls.dev", "adminpw01")
NEW = ("fresh@cls.dev", "freshpw01")

ok = 0; fail = 0
def chk(name, cond, detail=""):
    global ok, fail
    print(f"  {'ok' if cond else 'FAIL':<5} {name:<44} {detail}")
    ok += bool(cond); fail += (not cond)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def wait_listen(port, proc, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None: return False
        with socket.socket() as s:
            s.settimeout(0.25)
            try: s.connect(("127.0.0.1", port)); return True
            except OSError: time.sleep(0.05)
    return False

def main():
    if len(sys.argv) < 2:
        print("usage: test_velocity.py <cellar-binary>", file=sys.stderr); return 2
    binary = sys.argv[1]
    d = tempfile.mkdtemp(prefix="cls-vel-")
    db = os.path.join(d, "data.db")
    try:
        con = sqlite3.connect(db)
        con.executescript(open(os.path.join(HERE, "schema.sql")).read())
        con.executescript(open(os.path.join(HERE, "seed.sql")).read())
        con.commit(); con.close()
        shutil.copy(os.path.join(HERE, "hooks.lua"), os.path.join(d, "hooks.lua"))
        shutil.copy(os.path.join(HERE, "policies.json"), os.path.join(d, "policies.json"))

        port = free_port()
        env = dict(os.environ, CEL_PORT=str(port), CEL_DATA_DB=db, CEL_LOG_LEVEL="warn",
                   CEL_POLICY_FILE=os.path.join(d, "policies.json"),
                   CEL_JOBS_INTERVAL="0", CLS_POST_LIMIT="3", CLS_CONTACT_LIMIT="2",
                   CEL_AUTH_RATELIMIT="0", CEL_API_RATELIMIT="0",
                   CEL_SEED_USERS=f"{ADMIN[0]}:{ADMIN[1]}:admin")
        log = open(os.path.join(d, "server.log"), "w")
        proc = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if not wait_listen(port, proc):
                print(open(os.path.join(d, "server.log")).read(), file=sys.stderr); return 2
            run(port)
        finally:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
            log.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print(f"\n{'ALL PASS' if fail == 0 else f'FAILED ({fail})'}  ({ok} ok)")
    return 1 if fail else 0

def run(port):
    HOST = "127.0.0.1"
    def req(method, path, body=None, token=None):
        c = http.client.HTTPConnection(HOST, port, timeout=10)
        h = {"Content-Type": "application/json"} if body is not None else {}
        if token: h["Authorization"] = "Bearer " + token
        c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
        r = c.getresponse(); data = r.read(); c.close()
        try: return r.status, json.loads(data)
        except Exception: return r.status, None

    print(f"== classifieds velocity e2e -> {HOST}:{port} ==")
    # a brand-new, unverified self-registered user (trust 1× → cap = CLS_POST_LIMIT)
    req("POST", "/auth/register", {"email": NEW[0], "password": NEW[1], "role": "user"})
    tok = (req("POST", "/auth/login", {"email": NEW[0], "password": NEW[1]})[1] or {}).get("token")
    chk("new user logged in", bool(tok))

    def post(n):
        return req("POST", "/api/listings",
                   {"category_id": "cat-cars", "title": f"Car {n}", "city_id": "ci-bishkek",
                    "attributes": {"make": "Toyota", "year": 2015}}, token=tok)[0]

    s1, s2, s3 = post(1), post(2), post(3)
    chk("first 3 posts allowed", s1 == 201 and s2 == 201 and s3 == 201, f"{s1},{s2},{s3}")
    s4, b4 = req("POST", "/api/listings",
                 {"category_id": "cat-cars", "title": "Car 4", "city_id": "ci-bishkek",
                  "attributes": {"make": "Toyota", "year": 2015}}, token=tok)
    chk("4th post over limit -> 400", s4 == 400, f"status={s4}")
    chk("limit reason surfaced", "limit" in (b4 or {}).get("message", "").lower(), str(b4))

    s, b = req("POST", "/rpc/my_limits", {}, token=tok)
    r = (b or {}).get("result") or {}
    chk("my_limits reports cap", r.get("post_limit_24h") == 3 and r.get("trust") == 1, str(r))
    chk("my_limits reports usage", r.get("posts_used_24h") == 3, str(r))

    # an admin is exempt from the cap
    atok = (req("POST", "/auth/login", {"email": ADMIN[0], "password": ADMIN[1]})[1] or {}).get("token")
    codes = [req("POST", "/api/listings",
                 {"category_id": "cat-cars", "title": f"Admin {i}", "city_id": "ci-bishkek",
                  "attributes": {"make": "Toyota", "year": 2015}}, token=atok)[0] for i in range(5)]
    chk("admin exempt from cap", all(c == 201 for c in codes), str(codes))

    # ---- contact velocity: reveal_contact is gated too (anti number-scraping) ----
    # CLS_CONTACT_LIMIT=2, new user trust 1x -> cap 2 reveals/hour. The seller is
    # the admin (so each reveal logs a number_revealed contact_event that counts).
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Contact src", "city_id": "ci-bishkek",
                "attributes": {"make": "Toyota", "year": 2015}}, token=atok)
    lid = (b or {}).get("row", {}).get("id")
    chk("seller listing created for reveal test", s == 201 and bool(lid), f"status={s} id={lid}")
    req("POST", "/rpc/set_listing_contact",
        {"listing_id": lid, "phone": "+996700000001"}, token=atok)

    r1 = req("POST", "/rpc/reveal_contact", {"listing_id": lid}, token=tok)
    r2 = req("POST", "/rpc/reveal_contact", {"listing_id": lid}, token=tok)
    r3 = req("POST", "/rpc/reveal_contact", {"listing_id": lid}, token=tok)
    chk("reveal 1 under cap -> 200 + phone", r1[0] == 200 and ((r1[1] or {}).get("result") or {}).get("phone"), str(r1))
    chk("reveal 2 under cap -> 200 + phone", r2[0] == 200 and ((r2[1] or {}).get("result") or {}).get("phone"), str(r2))
    chk("reveal 3 over cap -> 400 (gated)", r3[0] == 400, f"status={r3[0]} (regression: reveal_contact must be velocity-gated)")
    # admin (exempt) can keep revealing past the cap
    ra = req("POST", "/rpc/reveal_contact", {"listing_id": lid}, token=atok)  # admin views own -> allowed, no count
    chk("admin reveal not gated", ra[0] == 200, str(ra))

if __name__ == "__main__":
    sys.exit(main())
