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
        # build the app db: bundle schema + the REAL bundle seed (so a seed
        # regression is caught here too)
        con = sqlite3.connect(db)
        con.executescript(open(os.path.join(HERE, "schema.sql")).read())
        con.executescript(open(os.path.join(HERE, "seed.sql")).read())
        con.commit(); con.close()
        # the bundle hooks + policies live beside data.db (single-app mode)
        shutil.copy(os.path.join(HERE, "hooks.lua"), os.path.join(d, "hooks.lua"))
        shutil.copy(os.path.join(HERE, "policies.json"), os.path.join(d, "policies.json"))

        port = free_port()
        # Single-app mode loads the policy from CEL_POLICY_FILE (multi-app loads
        # each bundle's policies.json automatically). Point it at the bundle's so
        # the real policy is enforced — anon browse allowed, etc.
        env = dict(os.environ,
                   CEL_PORT=str(port), CEL_DATA_DB=db, CEL_LOG_LEVEL="warn",
                   CEL_POLICY_FILE=os.path.join(d, "policies.json"),
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

    # ---- A1.2 post-form contract: category_form by slug ----
    s, b = req("POST", "/rpc/category_form", {"category": "cars"}, token=tok)
    form = (b or {}).get("result") or {}
    chk("category_form -> 200", s == 200, f"status={s}")
    chk("form category is cars", (form.get("category") or {}).get("slug") == "cars", str(form.get("category")))
    chk("breadcrumb root->leaf",
        [c.get("slug") for c in form.get("breadcrumb", [])] == ["transport", "cars"],
        str(form.get("breadcrumb")))
    attrs = {a["key"]: a for a in form.get("attributes", [])}
    chk("form has make attr", "make" in attrs and attrs["make"]["required"] is True, str(list(attrs)))
    chk("make options is array w/ Toyota", "Toyota" in (attrs.get("make", {}).get("options") or []),
        str(attrs.get("make", {}).get("options")))

    car = {"category_id": "cat-cars", "title": "Toyota Camry", "price": 150000,
           "city_id": "ci-bishkek", "district_id": "di-leninsky"}

    # ---- valid car create -> 201 + facets for the filterable attrs ----
    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Toyota", "year": 2015, "mileage": 120000,
                                     "transmission": "Автомат", "fuel": "Бензин",
                                     "body": "Седан"}), token=tok)
    chk("valid car create -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    row = (b or {}).get("row", {}); lid = row.get("id")
    chk("server set seller_id", bool(row.get("seller_id")), str(row.get("seller_id")))
    chk("server set expires_at", bool(row.get("expires_at")), str(row.get("expires_at")))

    f = facets(db, lid) if lid else {}
    chk("facet make (text)", f.get("make") == (None, "Toyota"), str(f.get("make")))
    chk("facet year (num)",  f.get("year") == (2015.0, None), str(f.get("year")))
    chk("facet mileage (num)", f.get("mileage") == (120000.0, None), str(f.get("mileage")))
    chk("facet transmission (text)", f.get("transmission") == (None, "Автомат"), str(f.get("transmission")))
    chk("optional model not faceted (absent)", "model" not in f, str(list(f.keys())))

    # ---- missing required attr -> 400 (year is required for cars) ----
    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Toyota"}), token=tok)
    chk("missing required -> 400", s == 400, f"status={s}")
    chk("reason mentions year", "year" in (b or {}).get("message", ""), str(b))

    # ---- bad enum -> 400 (Жигуль not in the make list) ----
    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Жигуль", "year": 2015}), token=tok)
    chk("bad enum -> 400", s == 400, f"status={s}")
    chk("reason mentions make", "make" in (b or {}).get("message", ""), str(b))

    # ---- wrong type -> 400 (year must be a number) ----
    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Toyota", "year": "old"}), token=tok)
    chk("wrong type -> 400", s == 400, f"status={s}")

    # ---- non-filterable attr excluded (apartment.total_floors, filterable=0) ----
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-apartments", "title": "2-комн квартира", "price": 80000,
                "city_id": "ci-bishkek",
                "attributes": {"deal": "Аренда", "rooms": 2, "area": 55.5, "floor": 3,
                               "total_floors": 9, "furnished": True}}, token=tok)
    chk("valid apartment -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    aid = (b or {}).get("row", {}).get("id")
    af = facets(db, aid) if aid else {}
    chk("facet rooms (num)", af.get("rooms") == (2.0, None), str(af.get("rooms")))
    chk("facet area (num)",  af.get("area") == (55.5, None), str(af.get("area")))
    chk("facet furnished (bool->1)", af.get("furnished") == (1.0, None), str(af.get("furnished")))
    chk("non-filterable total_floors excluded", "total_floors" not in af, str(list(af.keys())))

    # ---- A1.1 photos attach + validation ----
    pid_a, pid_b = "a" * 32, "b" * 32
    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Toyota", "year": 2018},
                    photos=[pid_a, pid_b]), token=tok)
    chk("create with photos -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    ph = (b or {}).get("row", {}).get("photos")
    if isinstance(ph, str):
        try: ph = json.loads(ph)
        except Exception: pass
    chk("photos stored in order", ph == [pid_a, pid_b], str((b or {}).get("row", {}).get("photos")))

    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Toyota", "year": 2018},
                    photos=[("%032x" % i) for i in range(13)]), token=tok)
    chk("too many photos -> 400", s == 400, f"status={s}")

    s, b = req("POST", "/api/listings",
               dict(car, attributes={"make": "Toyota", "year": 2018},
                    photos=["not-a-valid-id"]), token=tok)
    chk("bad photo id -> 400", s == 400, f"status={s}")

    # ---- update re-syncs facets (idempotent rebuild) ----
    s, b = req("PATCH", f"/api/listings/{lid}",
               {"category_id": "cat-cars",
                "attributes": {"make": "Honda", "year": 2016, "mileage": 90000}}, token=tok)
    chk("update -> 200", s == 200, f"status={s} {b if s!=200 else ''}")
    f = facets(db, lid)
    chk("facet make resynced", f.get("make") == (None, "Honda"), str(f.get("make")))
    chk("facet year resynced", f.get("year") == (2016.0, None), str(f.get("year")))
    chk("dropped attr gone (transmission)", "transmission" not in f, str(list(f.keys())))

    # ---- admin rpc: rebuild_facets reconciles ----
    s, b = req("POST", "/rpc/rebuild_facets", {"id": lid}, token=tok)
    chk("rpc rebuild_facets -> 200", s == 200, f"status={s}")
    chk("rpc reports 3 facets", ((b or {}).get("result") or {}).get("facets") == 3, str(b))

    # ---- A1.3 full-text search (FTS5) ----
    def mklisting(title, cat, attrs):
        s, b = req("POST", "/api/listings",
                   {"category_id": cat, "title": title, "price": 100, "city_id": "ci-bishkek",
                    "attributes": attrs}, token=tok)
        return s, (b or {}).get("row", {}).get("id")

    s, _ = mklisting("Toyota Camry 2015", "cat-cars", {"make": "Toyota", "year": 2015})
    chk("search seed car1 -> 201", s == 201, f"status={s}")
    mklisting("Тойота Королла", "cat-cars", {"make": "Toyota", "year": 2016})
    s, apt_id = mklisting("Квартира в центре города", "cat-apartments", {"deal": "Продажа", "rooms": 3})
    chk("search seed apt -> 201", s == 201, f"status={s}")

    def search(q, **kw):
        s, b = req("POST", "/rpc/search", dict(q=q, **kw), token=tok)
        return s, (b or {}).get("result") or {}
    def titles(r): return [x["title"] for x in r.get("results", [])]

    s, r = search("toyota")
    chk("search 'toyota' -> 200", s == 200, f"status={s}")
    chk("finds Toyota Camry", "Toyota Camry 2015" in titles(r), str(titles(r)))

    s, r = search("тойота")
    chk("Cyrillic 'тойота' finds Королла", "Тойота Королла" in titles(r), str(titles(r)))
    s, r = search("ТОЙОТА")
    chk("case-folded 'ТОЙОТА' matches", "Тойота Королла" in titles(r), str(titles(r)))

    s, r = search("королл")
    chk("prefix 'королл' matches Королла", "Тойота Королла" in titles(r), str(titles(r)))

    s, r = search("квартира")
    chk("'квартира' finds apartment", "Квартира в центре города" in titles(r), str(titles(r)))

    # category narrowing
    s, r = search("тойота", category="cat-cars")
    chk("category narrows (cars has it)", r.get("total", 0) >= 1, str(r.get("total")))
    s, r = search("тойота", category="cat-apartments")
    chk("category narrows (apartments: none)", r.get("total") == 0, str(r.get("total")))

    # no match + injection safety (special chars must not error)
    s, r = search("zzzznomatchqqq")
    chk("no match -> empty", s == 200 and r.get("total") == 0, f"status={s} {r.get('total')}")
    s, r = search('"); drop table listings; --  *(^')
    chk("fts injection safe -> 200", s == 200, f"status={s}")

    # trigger sync on UPDATE: retitle the apartment, old term gone, new term found
    s, _ = req("PATCH", f"/api/listings/{apt_id}", {"title": "Студия уютная"}, token=tok)
    chk("retitle apartment -> 200", s == 200, f"status={s}")
    s, r = search("квартира")
    chk("old title no longer matches", "Студия уютная" not in titles(r) and apt_id not in [x["id"] for x in r.get("results", [])], str(titles(r)))
    s, r = search("студия")
    chk("new title matches after update", "Студия уютная" in titles(r), str(titles(r)))

    # trigger sync on DELETE: removed from the index
    s, _ = req("DELETE", f"/api/listings/{apt_id}", token=tok)
    chk("delete listing -> 200", s == 200, f"status={s}")
    s, r = search("студия")
    chk("deleted listing gone from search", apt_id not in [x["id"] for x in r.get("results", [])], str(titles(r)))

    # ---- A1.4 faceted filtering (fresh category cat-moto for exact counts) ----
    def mkmoto(title, make, year, cc):
        s, b = req("POST", "/api/listings",
                   {"category_id": "cat-moto", "title": title, "price": 200, "city_id": "ci-bishkek",
                    "attributes": {"make": make, "year": year, "engine_cc": cc}}, token=tok)
        return s
    chk("moto seed A", mkmoto("Honda CBR", "Honda", 2020, 600) == 201)
    mkmoto("Yamaha R3", "Yamaha", 2018, 400)
    mkmoto("Honda Africa Twin", "Honda", 2021, 1000)

    def facet_search(**kw):
        s, b = req("POST", "/rpc/search", kw, token=tok)
        return s, (b or {}).get("result") or {}

    # browse a category (no q) -> all active + facet counts
    s, r = facet_search(category="cat-moto", sort="newest")
    chk("browse cat-moto -> 3", r.get("total") == 3, str(r.get("total")))
    fac = r.get("facets") or {}
    make_counts = {x["value"]: x["count"] for x in fac.get("make", [])}
    chk("facet make Honda=2", make_counts.get("Honda") == 2, str(make_counts))
    chk("facet make Yamaha=1", make_counts.get("Yamaha") == 1, str(make_counts))

    # text/enum facet filter (make=Honda) -> 2
    s, r = facet_search(category="cat-moto", filters=[{"key": "make", "values": ["Honda"]}])
    chk("filter make=Honda -> 2", r.get("total") == 2, str(r.get("total")))

    # numeric range filter (year >= 2020) -> 2 (A + C)
    s, r = facet_search(category="cat-moto", filters=[{"key": "year", "min": 2020}])
    chk("filter year>=2020 -> 2", r.get("total") == 2, str(r.get("total")))

    # combined: make=Honda AND year>=2021 -> 1 (Africa Twin)
    s, r = facet_search(category="cat-moto",
                        filters=[{"key": "make", "values": ["Honda"]}, {"key": "year", "min": 2021}])
    chk("filter make+year -> 1", r.get("total") == 1, str(r.get("total")))
    chk("combined result is Africa Twin", titles(r) == ["Honda Africa Twin"], str(titles(r)))

    # OR within a key's values (Honda OR Yamaha) -> all 3
    s, r = facet_search(category="cat-moto", filters=[{"key": "make", "values": ["Honda", "Yamaha"]}])
    chk("filter make in (Honda,Yamaha) -> 3", r.get("total") == 3, str(r.get("total")))

    # facet counts reflect the BASE set (not narrowed by the make selection)
    s, r = facet_search(category="cat-moto", filters=[{"key": "make", "values": ["Yamaha"]}])
    chk("narrowed result -> 1", r.get("total") == 1, str(r.get("total")))
    mc = {x["value"]: x["count"] for x in (r.get("facets") or {}).get("make", [])}
    chk("sidebar still shows Honda=2", mc.get("Honda") == 2, str(mc))

    # sort price_asc doesn't error and returns the set
    s, r = facet_search(category="cat-moto", sort="price_asc")
    chk("sort price_asc -> 3", s == 200 and r.get("total") == 3, f"status={s} {r.get('total')}")

    # text query + facet together
    s, r = facet_search(q="honda", category="cat-moto", filters=[{"key": "year", "min": 2021}])
    chk("q='honda' + year>=2021 -> 1", r.get("total") == 1, str(r.get("total")))

    # ---- A1.5 browse + detail ----
    # browse by geo (city filter, no q) — all moto seeded in Bishkek
    s, r = facet_search(category="cat-moto", city="ci-bishkek")
    chk("browse by city -> 3", r.get("total") == 3, str(r.get("total")))
    s, r = facet_search(city="ci-bishkek")
    chk("browse city-only (no cat) non-empty", r.get("total", 0) >= 3, str(r.get("total")))

    # listing detail (anon) — via the public `listing` rpc (engine /api read path
    # requires auth; public reads go through rpcs, like search)
    one = (facet_search(category="cat-moto", limit=1)[1].get("results") or [{}])[0]
    s, b = req("POST", "/rpc/listing", {"id": one.get("id")})   # no token = anon
    detail = ((b or {}).get("result") or {}).get("listing") or {}
    chk("listing detail (anon) -> 200", s == 200, f"status={s}")
    chk("detail has title", bool(detail.get("title")), str(detail)[:80])
    chk("detail id matches", detail.get("id") == one.get("id"), str(detail.get("id")))
    # a non-existent id -> rpc returns nil -> 400
    s, b = req("POST", "/rpc/listing", {"id": "nope-not-real"})
    chk("unknown listing -> 400", s == 400, f"status={s}")

    # engine fix (#1): /api reads now defer to policy, so anon can read where the
    # policy lists 'anon' — public browse without an rpc workaround.
    s, b = req("GET", f"/api/listings/{one.get('id')}")          # anon, no token
    chk("anon /api detail -> 200 (policy)", s == 200, f"status={s}")
    s, b = req("GET", "/api/listings")                           # anon list
    chk("anon /api list -> 200", s == 200, f"status={s}")
    # a table that does NOT grant 'anon' stays protected (401 unauthenticated)
    s, b = req("GET", "/api/listing_facet")
    chk("anon /api listing_facet -> 401", s == 401, f"status={s}")
    # ...and an authenticated user without the role gets 403, not 401
    # (admin is superuser here, so just assert the table is reachable as admin)
    s, b = req("GET", "/api/listing_facet", token=tok)
    chk("admin /api listing_facet -> 200", s == 200, f"status={s}")

if __name__ == "__main__":
    sys.exit(main())
