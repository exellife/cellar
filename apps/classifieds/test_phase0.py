#!/usr/bin/env python3
"""classifieds Phase-0 e2e: the validation (A0.4) + facet-sync (A0.5) hooks on
the real write path.

Boots cellar single-app against a data.db built from this bundle's schema.sql +
a tiny seeded catalog (a 'cars' category with year/make/mileage/color attrs),
then drives the REST API:
  - a valid listing -> 201, and listing_facet is populated for the filterable
    attrs only (year/make/mileage), color (filterable=0) excluded;
  - invalid attributes (missing-required / bad-enum / wrong-type) -> 400, and
    nothing is written;
  - an update re-syncs the facets (idempotent rebuild in after()).

Usage: test_phase0.py <cellar-binary>
Facets are read back with a separate sqlite3 connection (WAL: committed reads).
"""
import os, sys, json, time, socket, sqlite3, subprocess, tempfile, shutil
import http.client

HERE = os.path.dirname(os.path.abspath(__file__))
ADMIN = ("admin@cls.dev", "adminpw01")

SEED = """
INSERT INTO category(id,slug,name) VALUES ('cat-cars','cars','Авто');
INSERT INTO category_attribute(id,category_id,key,label,type,required,filterable,options) VALUES
  ('at-year','cat-cars','year','Год','int',1,1,NULL),
  ('at-make','cat-cars','make','Марка','enum',1,1,'["Toyota","Honda","BMW"]'),
  ('at-mile','cat-cars','mileage','Пробег','int',0,1,NULL),
  ('at-color','cat-cars','color','Цвет','text',0,0,NULL);
INSERT INTO geo_oblast(id,name) VALUES ('ob-chuy','Чуйская область');
INSERT INTO geo_city(id,oblast_id,name) VALUES ('ci-bishkek','ob-chuy','Бишкек');
"""

ok = 0; fail = 0
def chk(name, cond, detail=""):
    global ok, fail
    print(f"  {'ok' if cond else 'FAIL':<5} {name:<46} {detail}")
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
        print("usage: test_phase0.py <cellar-binary>", file=sys.stderr); return 2
    binary = sys.argv[1]
    d = tempfile.mkdtemp(prefix="cls-p0-")
    db = os.path.join(d, "data.db")
    try:
        # build the app db: bundle schema + seeded catalog
        con = sqlite3.connect(db)
        con.executescript(open(os.path.join(HERE, "schema.sql")).read())
        con.executescript(SEED)
        con.commit(); con.close()
        # the bundle hooks + policies live beside data.db (single-app mode)
        shutil.copy(os.path.join(HERE, "hooks.lua"), os.path.join(d, "hooks.lua"))
        shutil.copy(os.path.join(HERE, "policies.json"), os.path.join(d, "policies.json"))

        port = free_port()
        env = dict(os.environ,
                   CEL_PORT=str(port), CEL_DATA_DB=db, CEL_LOG_LEVEL="warn",
                   CEL_AUTH_RATELIMIT="0", CEL_API_RATELIMIT="0",
                   CEL_SEED_USERS=f"{ADMIN[0]}:{ADMIN[1]}:admin")
        log = open(os.path.join(d, "server.log"), "w")
        proc = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if not wait_listen(port, proc):
                print("server did not start; log:", file=sys.stderr)
                print(open(os.path.join(d, "server.log")).read(), file=sys.stderr)
                return 2
            run_checks(port, db)
        finally:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
            log.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print(f"\n{'ALL PASS' if fail == 0 else f'FAILED ({fail})'}  ({ok} ok)")
    return 1 if fail else 0

def facets(db, listing_id):
    c = sqlite3.connect(db)
    rows = c.execute("SELECT key, num_value, text_value FROM listing_facet "
                     "WHERE listing_id=? ORDER BY key", (listing_id,)).fetchall()
    c.close()
    return {r[0]: (r[1], r[2]) for r in rows}

def run_checks(port, db):
    HOST = "127.0.0.1"
    def req(method, path, body=None, token=None):
        c = http.client.HTTPConnection(HOST, port, timeout=10)
        h = {}
        if body is not None: h["Content-Type"] = "application/json"
        if token: h["Authorization"] = "Bearer " + token
        c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=h)
        r = c.getresponse(); data = r.read(); c.close()
        try: parsed = json.loads(data)
        except Exception: parsed = None
        return r.status, parsed

    print(f"== classifieds Phase-0 hooks -> {HOST}:{port} ==")
    s, b = req("POST", "/auth/login", {"email": ADMIN[0], "password": ADMIN[1]})
    tok = (b or {}).get("token"); chk("login admin", bool(tok), f"status={s}")

    base = {"category_id": "cat-cars", "title": "Toyota Camry", "price": 150000,
            "city_id": "ci-bishkek"}

    # ---- valid create -> 201 + facets (filterable only) ----
    s, b = req("POST", "/api/listings",
               dict(base, attributes={"year": 2015, "make": "Toyota",
                                      "mileage": 120000, "color": "white"}), token=tok)
    chk("valid create -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    row = (b or {}).get("row", {}); lid = row.get("id")
    chk("server set seller_id", bool(row.get("seller_id")), str(row.get("seller_id")))
    chk("server set expires_at", bool(row.get("expires_at")), str(row.get("expires_at")))

    f = facets(db, lid) if lid else {}
    chk("facet year (num)",  f.get("year") == (2015.0, None), str(f.get("year")))
    chk("facet make (text)", f.get("make") == (None, "Toyota"), str(f.get("make")))
    chk("facet mileage (num)", f.get("mileage") == (120000.0, None), str(f.get("mileage")))
    chk("non-filterable color excluded", "color" not in f, str(list(f.keys())))

    # ---- missing required attr -> 400 ----
    s, b = req("POST", "/api/listings",
               dict(base, attributes={"make": "Toyota"}), token=tok)   # no year
    chk("missing required -> 400", s == 400, f"status={s}")
    chk("reason mentions year", "year" in (b or {}).get("message", ""), str(b))

    # ---- bad enum -> 400 ----
    s, b = req("POST", "/api/listings",
               dict(base, attributes={"year": 2015, "make": "Lada"}), token=tok)
    chk("bad enum -> 400", s == 400, f"status={s}")
    chk("reason mentions make", "make" in (b or {}).get("message", ""), str(b))

    # ---- wrong type -> 400 ----
    s, b = req("POST", "/api/listings",
               dict(base, attributes={"year": "old", "make": "Toyota"}), token=tok)
    chk("wrong type -> 400", s == 400, f"status={s}")

    # ---- update re-syncs facets (idempotent rebuild) ----
    s, b = req("PATCH", f"/api/listings/{lid}",
               {"category_id": "cat-cars",
                "attributes": {"year": 2016, "make": "Honda", "mileage": 90000}}, token=tok)
    chk("update -> 200", s == 200, f"status={s} {b if s!=200 else ''}")
    f = facets(db, lid)
    chk("facet year resynced", f.get("year") == (2016.0, None), str(f.get("year")))
    chk("facet make resynced", f.get("make") == (None, "Honda"), str(f.get("make")))
    chk("facet mileage resynced", f.get("mileage") == (90000.0, None), str(f.get("mileage")))

    # ---- admin rpc: rebuild_facets reconciles ----
    s, b = req("POST", "/rpc/rebuild_facets", {"id": lid}, token=tok)
    chk("rpc rebuild_facets -> 200", s == 200, f"status={s}")
    chk("rpc reports 3 facets", ((b or {}).get("result") or {}).get("facets") == 3, str(b))

if __name__ == "__main__":
    sys.exit(main())
