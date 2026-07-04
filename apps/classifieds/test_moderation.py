#!/usr/bin/env python3
"""classifieds trust & safety e2e: report -> moderation queue -> takedown.

Boots cellar single-app against the real bundle (schema.sql + seed.sql + hooks +
policies), then drives the moderation slice end to end:

  core flow (manual moderation, auto-hide OFF = default)
    - a user reports a listing; bad reason / own-listing are rejected;
    - a repeat report by the same user is a silent no-op (UNIQUE dedupe);
    - a non-admin cannot read the queue (the _rpc whitelist denies it);
    - the admin queue groups reports per listing with a count;
    - takedown flips the listing to 'removed' (no longer public), resolves its
      open reports as 'actioned', and notifies the seller;
    - reinstate brings it back to 'active' and public again.

  auto-hide flow (CLS_AUTO_HIDE_REPORTS=2)
    - one report leaves it active; a 2nd DISTINCT reporter auto-hides it
      ('pending', not public); admin reinstate restores it.

Notes: an rpc hook returning nil is encoded by the engine as HTTP 400; an empty
cellar.query result serializes to a JSON {} (not []) — assertions account for both.

Usage: test_moderation.py <cellar-binary>
"""
import os, sys, json, time, socket, sqlite3, subprocess, tempfile, shutil
import http.client

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = None
ADMIN  = ("admin@cls.dev",  "adminpw01")
SELLER = ("seller@cls.dev", "sellerpw01")
R1     = ("r1@cls.dev",     "r1pw0001")
R2     = ("r2@cls.dev",     "r2pw0001")

ok = 0; fail = 0
def chk(name, cond, detail=""):
    global ok, fail
    print(f"  {'ok' if cond else 'FAIL':<5} {name:<52} {detail}")
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

def boot(d, extra_env=None):
    db = os.path.join(d, "data.db")
    con = sqlite3.connect(db)
    con.executescript(open(os.path.join(HERE, "schema.sql")).read())
    con.executescript(open(os.path.join(HERE, "seed.sql")).read())
    con.commit(); con.close()
    shutil.copy(os.path.join(HERE, "hooks.lua"),    os.path.join(d, "hooks.lua"))
    shutil.copy(os.path.join(HERE, "policies.json"), os.path.join(d, "policies.json"))
    port = free_port()
    env = dict(os.environ,
               CEL_PORT=str(port), CEL_DATA_DB=db, CEL_LOG_LEVEL="warn",
               CEL_POLICY_FILE=os.path.join(d, "policies.json"),
               CEL_JOBS_INTERVAL="0",
               CLS_POST_LIMIT="1000", CLS_CONTACT_LIMIT="1000",
               CEL_AUTH_RATELIMIT="0", CEL_API_RATELIMIT="0",
               CEL_SEED_USERS=(f"{ADMIN[0]}:{ADMIN[1]}:admin;{SELLER[0]}:{SELLER[1]}:user;"
                               f"{R1[0]}:{R1[1]}:user;{R2[0]}:{R2[1]}:user"))
    if extra_env: env.update(extra_env)
    log = open(os.path.join(d, "server.log"), "w")
    proc = subprocess.Popen([BIN], env=env, stdout=log, stderr=subprocess.STDOUT)
    if not wait_listen(port, proc):
        print(open(os.path.join(d, "server.log")).read(), file=sys.stderr)
        raise SystemExit("server did not start")
    return port, proc, db, log

def mkreq(port):
    def req(method, path, body=None, token=None):
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        h = {}
        if body is not None: h["Content-Type"] = "application/json"
        if token: h["Authorization"] = "Bearer " + token
        c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
        r = c.getresponse(); data = r.read(); c.close()
        try: parsed = json.loads(data)
        except Exception: parsed = None
        return r.status, parsed
    return req

def login(req, creds):
    _, b = req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})
    return (b or {}).get("token")

def result(b):
    return (b or {}).get("result")

def post_listing(req, tok, title="Toyota Camry"):
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": title, "price": 150000,
                "city_id": "ci-bishkek", "district_id": "di-leninsky",
                "attributes": {"make": "Toyota", "year": 2018}}, token=tok)
    return s, (b or {}).get("row", {}).get("id")

def count(db, sql, args=()):
    c = sqlite3.connect(db); n = c.execute(sql, args).fetchone()[0]; c.close(); return n

def status_of(db, lid):
    c = sqlite3.connect(db); r = c.execute("SELECT status FROM listings WHERE id=?", (lid,)).fetchone(); c.close()
    return r[0] if r else None

def col(db, lid, name):   # name is a test-controlled literal
    c = sqlite3.connect(db); r = c.execute(f"SELECT {name} FROM listings WHERE id=?", (lid,)).fetchone(); c.close()
    return r[0] if r else None

def set_status(db, lid, st):
    c = sqlite3.connect(db); c.execute("UPDATE listings SET status=? WHERE id=?", (st, lid)); c.commit(); c.close()

def verify_user(db, email):
    c = sqlite3.connect(db)
    c.execute("UPDATE cel_users SET email_verified_at = strftime('%s','now') WHERE email=?", (email,))
    c.commit(); c.close()

def core_flow(port, db):
    req = mkreq(port)
    print(f"== core flow (manual moderation) -> 127.0.0.1:{port} ==")
    atok = login(req, ADMIN); stok = login(req, SELLER); r1 = login(req, R1); r2 = login(req, R2)
    chk("logins (admin/seller/r1/r2)", all([atok, stok, r1, r2]))

    s, lid = post_listing(req, stok)
    chk("seller posts listing -> 201", s == 201 and bool(lid), f"status={s}")

    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("anon sees the active listing", s == 200 and bool(result(b)) and bool(result(b).get("listing")), f"status={s}")

    s, b = req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "bogus"}, token=r1)
    chk("invalid reason rejected", s == 200 and result(b) and result(b).get("ok") is False, str(result(b)))

    s, b = req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "spam"}, token=stok)
    chk("cannot report own listing", result(b) and result(b).get("ok") is False, str(result(b)))

    s, b = req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "scam", "note": "fake seller"}, token=r1)
    chk("r1 report accepted", result(b) and result(b).get("ok") is True, str(result(b)))

    s, b = req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "scam"}, token=r1)
    chk("r1 repeat is a silent no-op (ok)", result(b) and result(b).get("ok") is True, str(result(b)))

    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "prohibited"}, token=r2)
    chk("dedupe held: exactly 2 report rows",
        count(db, "SELECT count(*) FROM listing_report WHERE listing_id=?", (lid,)) == 2)

    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("still public before takedown (no auto-hide)", s == 200 and result(b) and result(b).get("listing"), f"status={s}")

    s, b = req("POST", "/rpc/list_reports", {}, token=r1)
    chk("list_reports denied for non-admin", s in (401, 403), f"status={s}")

    s, b = req("POST", "/rpc/list_reports", {}, token=atok)
    reps = result(b).get("reports") if result(b) else None
    chk("admin queue shows the listing (reports=2)",
        bool(reps) and reps[0]["listing_id"] == lid and reps[0]["reports"] == 2, str(reps))

    s, b = req("POST", "/rpc/takedown_listing", {"listing_id": lid, "note": "prohibited item"}, token=atok)
    chk("takedown -> removed", result(b) and result(b).get("status") == "removed", str(result(b)))

    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("removed listing no longer public (400)", s != 200, f"status={s}")

    # #6: a removed (non-active) listing is not reportable, and the response is the
    # SAME 'no such listing' as a truly-absent row (no existence/ownership oracle).
    s, b = req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "spam"}, token=r2)
    chk("cannot report a removed listing (oracle collapsed)",
        result(b) and result(b).get("ok") is False and result(b).get("error") == "no such listing", str(result(b)))

    chk("db status = removed", status_of(db, lid) == "removed", status_of(db, lid))
    chk("open reports cleared -> actioned",
        count(db, "SELECT count(*) FROM listing_report WHERE listing_id=? AND status='open'", (lid,)) == 0 and
        count(db, "SELECT count(*) FROM listing_report WHERE listing_id=? AND status='actioned'", (lid,)) == 2)
    chk("seller notified of removal",
        count(db, "SELECT count(*) FROM notification WHERE type='listing_removed'") >= 1)

    s, b = req("POST", "/rpc/list_reports", {}, token=atok)
    chk("queue empty after takedown", not (result(b) or {}).get("reports"), str(result(b)))

    s, b = req("POST", "/rpc/reinstate_listing", {"listing_id": lid}, token=atok)
    chk("reinstate -> active", result(b) and result(b).get("status") == "active", str(result(b)))
    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("public again after reinstate", s == 200 and result(b) and result(b).get("listing"), f"status={s}")

# #8 (dismiss_reports + listing_reports coverage) + #2 (re-report after resolution)
def dismiss_flow(port, db):
    req = mkreq(port)
    print(f"== dismiss_reports + listing_reports drill-in + re-report -> 127.0.0.1:{port} ==")
    atok = login(req, ADMIN); stok = login(req, SELLER); r1 = login(req, R1)
    s, lid = post_listing(req, stok, "Fine listing")
    chk("post -> 201", s == 201 and bool(lid), f"status={s}")
    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "other", "note": "a mistake"}, token=r1)

    s, b = req("POST", "/rpc/listing_reports", {"listing_id": lid}, token=atok)
    reps = result(b).get("reports") if result(b) else None
    chk("listing_reports drill-in returns the report (admin)",
        bool(reps) and reps[0]["reason"] == "other" and reps[0]["status"] == "open", str(reps))
    s, b = req("POST", "/rpc/listing_reports", {"listing_id": lid}, token=r1)
    chk("listing_reports denied for non-admin", s in (401, 403), f"status={s}")

    s, b = req("POST", "/rpc/dismiss_reports", {"listing_id": lid}, token=atok)
    chk("dismiss_reports -> ok", result(b) and result(b).get("ok") is True, str(result(b)))
    chk("reports -> dismissed", count(db, "SELECT count(*) FROM listing_report WHERE listing_id=? AND status='dismissed'", (lid,)) == 1)
    chk("listing unchanged (still active) after dismiss", status_of(db, lid) == "active", status_of(db, lid))

    # #2: the same reporter can re-flag after their prior report was resolved
    s, b = req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "scam"}, token=r1)
    chk("re-report after resolution accepted", result(b) and result(b).get("ok") is True, str(result(b)))
    chk("a fresh OPEN report exists (reopen)",
        count(db, "SELECT count(*) FROM listing_report WHERE listing_id=? AND status='open'", (lid,)) == 1)

# #3 / #5: takedown records prior status; reinstate restores it (never resurrects sold)
def sold_flow(port, db):
    req = mkreq(port)
    print(f"== takedown preserves prior status; reinstate restores sold -> 127.0.0.1:{port} ==")
    atok = login(req, ADMIN); stok = login(req, SELLER)
    s, lid = post_listing(req, stok, "Sold item")
    chk("post -> 201", s == 201 and bool(lid), f"status={s}")
    set_status(db, lid, "sold")               # seller marked it sold (owner PATCH path is incidental here)

    s, b = req("POST", "/rpc/takedown_listing", {"listing_id": lid}, token=atok)
    chk("takedown of a sold listing -> removed", result(b) and result(b).get("status") == "removed", str(result(b)))
    chk("prior 'sold' recorded in pre_moderation_status", col(db, lid, "pre_moderation_status") == "sold",
        col(db, lid, "pre_moderation_status"))

    s, b = req("POST", "/rpc/reinstate_listing", {"listing_id": lid}, token=atok)
    chk("reinstate restores 'sold' (NOT resurrected to active)", result(b) and result(b).get("status") == "sold", str(result(b)))
    chk("db status = sold", status_of(db, lid) == "sold", status_of(db, lid))
    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("restored-sold not public", s != 200, f"status={s}")

    # double takedown is a silent no-op (idempotent) — no error
    set_status(db, lid, "removed")
    s, b = req("POST", "/rpc/takedown_listing", {"listing_id": lid}, token=atok)
    chk("double takedown is a no-op ok", result(b) and result(b).get("status") == "removed", str(result(b)))

    # reinstate is a no-op unless under a hold
    set_status(db, lid, "active")
    s, b = req("POST", "/rpc/reinstate_listing", {"listing_id": lid}, token=atok)
    chk("reinstate of a non-held listing is rejected", result(b) and result(b).get("ok") is False, str(result(b)))

# #9 (single account can't trip auto-hide) + #1 (seller can't self-un-hide) + #4 (dismiss un-hides)
def autohide_flow(port, db):
    req = mkreq(port)
    print(f"== auto-hide (CLS_AUTO_HIDE_REPORTS=2): anti-brigade + self-un-hide + dismiss un-hide -> 127.0.0.1:{port} ==")
    atok = login(req, ADMIN); stok = login(req, SELLER); r1 = login(req, R1); r2 = login(req, R2)
    s, lid = post_listing(req, stok, "Suzuki Swift")
    chk("post -> 201", s == 201 and bool(lid), f"status={s}")

    # #9: one account reporting twice must NOT reach the threshold (dedupe holds)
    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "spam"}, token=r1)
    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "spam"}, token=r1)
    chk("single account (2 reports) does NOT auto-hide", status_of(db, lid) == "active", status_of(db, lid))

    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "scam"}, token=r2)
    chk("2nd DISTINCT reporter auto-hides (pending)", status_of(db, lid) == "pending", status_of(db, lid))
    chk("pre_moderation_status recorded 'active'", col(db, lid, "pre_moderation_status") == "active", col(db, lid, "pre_moderation_status"))
    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("auto-hidden not public (400)", s != 200, f"status={s}")

    # #1: seller cannot self-un-hide a 'pending' hold via renew_listing
    s, b = req("POST", "/rpc/renew_listing", {"listing_id": lid}, token=stok)
    chk("renew_listing refused on 'pending' (no self-un-hide)", s != 200 or not result(b), f"status={s}")
    chk("still pending after renew attempt", status_of(db, lid) == "pending", status_of(db, lid))

    req("POST", "/rpc/reinstate_listing", {"listing_id": lid}, token=atok)
    chk("admin reinstate -> active", status_of(db, lid) == "active", status_of(db, lid))
    s, b = req("POST", "/rpc/listing", {"id": lid})
    chk("public after reinstate", s == 200 and result(b) and result(b).get("listing"), f"status={s}")

    # #4: re-hide, then dismiss must UN-HIDE (not strand it in 'pending')
    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "spam"}, token=r1)
    req("POST", "/rpc/report_listing", {"listing_id": lid, "reason": "scam"}, token=r2)
    chk("re-hidden to pending", status_of(db, lid) == "pending", status_of(db, lid))
    req("POST", "/rpc/dismiss_reports", {"listing_id": lid}, token=atok)
    chk("dismiss un-hides the auto-hidden listing (#4)", status_of(db, lid) == "active", status_of(db, lid))

# item 7: email_verified gate on contact actions (reveal + start chat); posting stays open
def contact_gate_flow(port, db):
    req = mkreq(port)
    print(f"== contact gate: verified email required to reveal/chat (posting open) -> 127.0.0.1:{port} ==")
    stok = login(req, SELLER); r1 = login(req, R1)
    s, lid = post_listing(req, stok, "Gated-contact item")
    chk("posting is NOT gated (unverified seller posts) -> 201", s == 201 and bool(lid), f"status={s}")
    req("POST", "/rpc/set_listing_contact", {"listing_id": lid, "phone": "+996700000000"}, token=stok)

    # R1 is unverified -> both contact actions return the stable gate signal
    s, b = req("POST", "/rpc/reveal_contact", {"listing_id": lid}, token=r1)
    chk("unverified reveal -> email_not_verified",
        result(b) and result(b).get("ok") is False and result(b).get("error") == "email_not_verified", str(result(b)))
    s, b = req("POST", "/rpc/start_conversation", {"listing_id": lid}, token=r1)
    chk("unverified start_conversation -> email_not_verified",
        result(b) and result(b).get("error") == "email_not_verified", str(result(b)))

    # verify R1's email -> reveal now returns the phone
    verify_user(db, R1[0])
    s, b = req("POST", "/rpc/reveal_contact", {"listing_id": lid}, token=r1)
    chk("verified reveal returns the phone", result(b) and result(b).get("phone") == "+996700000000", str(result(b)))

def run(extra_env, fn):
    d = tempfile.mkdtemp(prefix="cls-mod-")
    try:
        port, proc, db, log = boot(d, extra_env)
        try: fn(port, db)
        finally:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
            log.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)

def main():
    global BIN
    if len(sys.argv) < 2:
        print("usage: test_moderation.py <cellar-binary>", file=sys.stderr); return 2
    BIN = sys.argv[1]
    run(None, core_flow)
    run(None, dismiss_flow)
    run(None, sold_flow)
    run(None, contact_gate_flow)   # gate on by default
    run({"CLS_AUTO_HIDE_REPORTS": "2"}, autohide_flow)
    print(f"\n{'ALL PASS' if fail == 0 else f'FAILED ({fail})'}  ({ok} ok)")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
