#!/usr/bin/env python3
"""classifieds background-worker e2e: prove jobs run AUTONOMOUSLY (no /jobs/run).

Boots cellar with CEL_JOBS_INTERVAL=1 (worker ticks every second), creates a
past-expiry active listing, enqueues an expire_listings job, then waits — WITHOUT
hitting /jobs/run — and asserts the worker thread claimed + ran the job (the
listing flips to 'expired' and the one-shot job is consumed).

Usage: test_worker.py <cellar-binary>
"""
import os, sys, json, time, socket, sqlite3, subprocess, tempfile, shutil
import http.client

HERE = os.path.dirname(os.path.abspath(__file__))
ADMIN = ("admin@cls.dev", "adminpw01")
SELLER = ("seller@cls.dev", "sellerpw01")

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
        print("usage: test_worker.py <cellar-binary>", file=sys.stderr); return 2
    binary = sys.argv[1]
    d = tempfile.mkdtemp(prefix="cls-worker-")
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
                   CEL_JOBS_INTERVAL="1",                  # autonomous worker, 1s tick
                   CEL_AUTH_RATELIMIT="0", CEL_API_RATELIMIT="0",
                   CEL_SEED_USERS=f"{ADMIN[0]}:{ADMIN[1]}:admin;{SELLER[0]}:{SELLER[1]}:user")
        log = open(os.path.join(d, "server.log"), "w")
        proc = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if not wait_listen(port, proc):
                print(open(os.path.join(d, "server.log")).read(), file=sys.stderr); return 2
            run(port, db)
        finally:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
            log.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print(f"\n{'ALL PASS' if fail == 0 else f'FAILED ({fail})'}  ({ok} ok)")
    return 1 if fail else 0

def run(port, db):
    HOST = "127.0.0.1"
    def req(method, path, body=None, token=None):
        c = http.client.HTTPConnection(HOST, port, timeout=10)
        h = {"Content-Type": "application/json"} if body is not None else {}
        if token: h["Authorization"] = "Bearer " + token
        c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
        r = c.getresponse(); data = r.read(); c.close()
        try: return r.status, json.loads(data)
        except Exception: return r.status, None
    def tok(creds):
        return (req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})[1] or {}).get("token")

    print(f"== classifieds worker e2e -> {HOST}:{port} ==")
    admin, seller = tok(ADMIN), tok(SELLER)
    chk("logins", bool(admin) and bool(seller))

    # a past-expiry active listing
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Auto-expire me", "city_id": "ci-bishkek",
                "attributes": {"make": "Toyota", "year": 2015}}, token=seller)
    lid = (b or {}).get("row", {}).get("id")
    chk("listing created", s == 201 and bool(lid), f"status={s}")
    req("PATCH", f"/api/listings/{lid}", {"expires_at": "2000-01-01T00:00:00Z"}, token=seller)

    # enqueue the sweep — then DON'T call /jobs/run; the worker thread must run it
    s, b = req("POST", "/rpc/enqueue_job", {"type": "expire_listings"}, token=admin)
    chk("enqueue_job", s == 200 and ((b or {}).get("result") or {}).get("id", 0) > 0, str(b))

    def status_of():
        c = sqlite3.connect(db)
        try: r = c.execute("SELECT status FROM listings WHERE id=?", (lid,)).fetchone(); return r[0] if r else None
        finally: c.close()
    def jobs_left():
        c = sqlite3.connect(db)
        try: return c.execute("SELECT count(*) FROM job").fetchone()[0]
        finally: c.close()

    # poll up to ~5s for the autonomous worker (1s tick) to process it
    expired = False
    for _ in range(50):
        if status_of() == "expired":
            expired = True; break
        time.sleep(0.1)
    chk("worker auto-expired the listing (no /jobs/run)", expired, f"status={status_of()}")
    chk("worker consumed the one-shot job", jobs_left() == 0, f"jobs_left={jobs_left()}")

if __name__ == "__main__":
    sys.exit(main())
