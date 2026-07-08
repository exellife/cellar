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
import http.client, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ADMIN = ("admin@cls.dev", "adminpw01")
BUYER = ("buyer@cls.dev", "buyerpw01")
SELLER = ("seller@cls.dev", "sellerpw01")
STRANGER = ("stranger@cls.dev", "strangerpw1")

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
                   CEL_JOBS_INTERVAL="0",   # drive jobs via /jobs/run deterministically
                   CLS_POST_LIMIT="1000", CLS_CONTACT_LIMIT="1000",   # don't trip velocity gates here
                   CLS_REQUIRE_VERIFIED="0",   # seeded users unverified; verify-gate tested in test_moderation
                   CEL_AUTH_RATELIMIT="0", CEL_API_RATELIMIT="0",
                   CEL_SEED_USERS=(f"{ADMIN[0]}:{ADMIN[1]}:admin;{BUYER[0]}:{BUYER[1]}:user;"
                                   f"{SELLER[0]}:{SELLER[1]}:user;{STRANGER[0]}:{STRANGER[1]}:user"))
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
    # localization (FEEDBACK issue 1): every breadcrumb node carries `labels` (KY-first UI
    # renders labels[locale] ?? name). cat-transport is seeded with a ky label; verify it flows.
    def _ky(node):
        l = node.get("labels")
        if isinstance(l, str):
            try: l = json.loads(l)
            except Exception: l = {}
        return (l or {}).get("ky")
    _bc = form.get("breadcrumb") or []
    _tn = next((n for n in _bc if n.get("slug") == "transport"), {})
    # transport is seeded with a ky label -> it must now flow into the breadcrumb (the fix).
    chk("category_form: breadcrumb carries labels (transport ky)", _ky(_tn) == "Транспорт", str(_tn))
    # a category WITH a label also exposes it on the category node (call it directly).
    s, b = req("POST", "/rpc/category_form", {"category": "transport"}, token=tok)
    chk("category_form: category node carries ky label",
        _ky(((b or {}).get("result") or {}).get("category") or {}) == "Транспорт",
        str(((b or {}).get("result") or {}).get("category")))
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
                "attributes": {"deal": "Аренда долгосрочная", "rooms": 2, "area": 55.5, "floor": 3,
                               "total_floors": 9, "furnished": True}}, token=tok)
    chk("valid apartment -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    aid = (b or {}).get("row", {}).get("id")
    af = facets(db, aid) if aid else {}
    chk("facet rooms (num)", af.get("rooms") == (2.0, None), str(af.get("rooms")))
    chk("facet area (num)",  af.get("area") == (55.5, None), str(af.get("area")))
    chk("facet furnished (bool->1)", af.get("furnished") == (1.0, None), str(af.get("furnished")))
    chk("non-filterable total_floors excluded", "total_floors" not in af, str(list(af.keys())))

    # ---- new taxonomy (FEEDBACK ask 1): a new subcategory's attributes drive the form + validation ----
    s, b = req("POST", "/rpc/category_form", {"category": "clothing"}, token=tok)
    cf = (b or {}).get("result") or {}
    cattrs = {a["key"]: a for a in cf.get("attributes", [])}
    chk("new subcat category_form (clothing)",
        (cf.get("category") or {}).get("slug") == "clothing" and cattrs.get("gender", {}).get("required") is True, str(list(cattrs)))
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-clothing", "title": "Куртка", "city_id": "ci-bishkek",
                "attributes": {"gender": "Мужская", "size": "L"}}, token=tok)
    cid = (b or {}).get("row", {}).get("id")
    chk("post in new subcat (clothing) -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    chk("new subcat facet (gender)", (facets(db, cid) if cid else {}).get("gender") == (None, "Мужская"), "")
    s, _ = req("POST", "/api/listings",
               {"category_id": "cat-clothing", "title": "No gender", "city_id": "ci-bishkek",
                "attributes": {"size": "L"}}, token=tok)
    chk("new subcat missing required (gender) -> 400", s == 400, f"status={s}")

    # ---- admin taxonomy writes + validation (FEEDBACK ask 3) ----
    s, b = req("POST", "/api/category",
               {"id": "cat-test-x", "slug": "test-x", "name": "Тест", "parent_id": "cat-home"}, token=tok)
    chk("admin create category -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    s, _ = req("POST", "/api/category", {"id": "cat-bad", "slug": "BadSlug", "name": "x"}, token=tok)
    chk("category bad slug -> 400", s == 400, f"status={s}")
    s, _ = req("POST", "/api/category", {"id": "cat-bad2", "slug": "bad2", "name": "x", "parent_id": "cat-nope"}, token=tok)
    chk("category bad parent -> 400", s == 400, f"status={s}")
    # attribute integrity
    s, _ = req("POST", "/api/category_attribute",
               {"id": "ca-x1", "category_id": "cat-test-x", "key": "kind", "label": "Тип", "type": "enum"}, token=tok)
    chk("enum without options -> 400", s == 400, f"status={s}")
    s, _ = req("POST", "/api/category_attribute",
               {"id": "ca-x2", "category_id": "cat-test-x", "key": "kind", "label": "Тип", "type": "enum", "options": "not json"}, token=tok)
    chk("bad options JSON -> 400", s == 400, f"status={s}")
    s, _ = req("POST", "/api/category_attribute",
               {"id": "ca-x6", "category_id": "cat-test-x", "key": "z", "label": "Z", "type": "bogus"}, token=tok)
    chk("bad attribute type -> 400", s == 400, f"status={s}")
    s, b = req("POST", "/api/category_attribute",
               {"id": "ca-x3", "category_id": "cat-test-x", "key": "kind", "label": "Тип", "type": "enum", "options": "[\"A\",\"B\"]", "filterable": 1}, token=tok)
    chk("valid enum attribute -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    s, _ = req("POST", "/api/category_attribute",
               {"id": "ca-x4", "category_id": "cat-test-x", "key": "sub", "label": "Под", "type": "text", "depends_on": "nope"}, token=tok)
    chk("depends_on nonexistent key -> 400", s == 400, f"status={s}")
    s, b = req("POST", "/api/category_attribute",
               {"id": "ca-x5", "category_id": "cat-test-x", "key": "sub", "label": "Под", "type": "text", "depends_on": "kind"}, token=tok)
    chk("depends_on existing key -> 201", s == 201, f"status={s} {b if s!=201 else ''}")
    # ask-3 update-path hardening (review): enum-flip needs options; depends_on needs category_id
    s, _ = req("PATCH", "/api/category_attribute/ca-x5", {"type": "enum"}, token=tok)
    chk("PATCH type->enum without options -> 400", s == 400, f"status={s}")
    s, _ = req("PATCH", "/api/category_attribute/ca-x5", {"depends_on": "kind"}, token=tok)
    chk("PATCH depends_on without category_id -> 400", s == 400, f"status={s}")

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

    # subtree-inclusive category browse (FEEDBACK 2026-07-01): a PARENT node returns
    # its descendants' listings even though it holds none directly. cat-transport is
    # the parent of cat-cars (see the category_form breadcrumb above); the toyotas
    # live in cat-cars, so browsing the parent must still find them.
    s, rp = search("тойота", category="cat-transport")
    s2, rc = search("тойота", category="cat-cars")
    chk("parent category returns the subtree (cat-transport ⊇ cat-cars)",
        rp.get("total", 0) >= 1 and rp.get("total", 0) >= rc.get("total", 0),
        f"parent={rp.get('total')} leaf={rc.get('total')}")
    chk("parent node holds no direct listings yet still yields results",
        rp.get("total", 0) >= 1 and all(x["category_id"] != "cat-transport" for x in rp.get("results", [])),
        str([x["category_id"] for x in rp.get("results", [])]))
    s, rn = search("тойота", category="cat-nonexistent")
    chk("unknown category still returns 0 (not the whole subtree)", rn.get("total") == 0, str(rn.get("total")))

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

    # engine fix (#1): /api reads defer to policy — anon may read a table that
    # lists 'anon' (the public catalog) but not one that doesn't.
    s, b = req("GET", "/api/category")                           # anon; category lists 'anon'
    chk("anon /api category -> 200 (public catalog)", s == 200, f"status={s}")
    s, b = req("GET", "/api/listing_facet")                      # internal table
    chk("anon /api listing_facet -> 401", s == 401, f"status={s}")
    # security fix (#1): /api/listings is now OWNER-scoped, not public — anon is
    # denied; public browse/detail go through the search + listing rpcs (active-only).
    s, b = req("GET", f"/api/listings/{one.get('id')}")          # anon
    chk("anon /api/listings get denied -> 401", s == 401, f"status={s}")
    s, b = req("GET", "/api/listings")                           # anon list
    chk("anon /api/listings list denied -> 401", s == 401, f"status={s}")

    # ---- A1.6 contact: phone-reveal (login-gated) + events ----
    buyer = (req("POST", "/auth/login", {"email": BUYER[0], "password": BUYER[1]})[1] or {}).get("token")
    chk("login buyer", bool(buyer))

    # seller (admin here) posts a listing with channels + sets the number
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Honda for contact test", "price": 5000,
                "city_id": "ci-bishkek", "allow_call": 1, "allow_whatsapp": 1,
                "attributes": {"make": "Honda", "year": 2019}}, token=tok)
    clid = (b or {}).get("row", {}).get("id")
    chk("contact listing -> 201", s == 201, f"status={s}")
    s, b = req("POST", "/rpc/set_listing_contact",
               {"listing_id": clid, "phone": "+996700123456", "whatsapp": "+996700123456"}, token=tok)
    chk("owner set_listing_contact -> 200", s == 200 and ((b or {}).get("result") or {}).get("ok"), str(b))

    # phone must NOT leak via the public listing read (the `listing` rpc; the
    # generic /api/listings is owner-scoped, so anon uses the rpc)
    s, b = req("POST", "/rpc/listing", {"id": clid})         # anon
    det = ((b or {}).get("result") or {}).get("listing") or {}
    chk("public detail has channel flags", det.get("allow_call") == 1, str(det)[:80])
    chk("public detail does NOT leak phone", "phone" not in det, str(list(det.keys())))

    # reveal requires login — now enforced at the engine _rpc gate (401) before the
    # hook's own self-guard runs (reveal_contact is whitelisted for "user" only).
    s, b = req("POST", "/rpc/reveal_contact", {"listing_id": clid})   # anon
    chk("reveal anon -> 401 (engine _rpc gate)", s == 401, f"status={s}")

    # buyer reveals phone -> gets number + event logged
    s, b = req("POST", "/rpc/reveal_contact", {"listing_id": clid}, token=buyer)
    rev = (b or {}).get("result") or {}
    chk("buyer reveal phone -> 200", s == 200, f"status={s}")
    chk("reveal returns number", rev.get("phone") == "+996700123456", str(rev))

    # whatsapp channel
    s, b = req("POST", "/rpc/reveal_contact", {"listing_id": clid, "channel": "whatsapp"}, token=buyer)
    chk("buyer reveal whatsapp", ((b or {}).get("result") or {}).get("whatsapp") == "+996700123456", str(b))

    # non-owner cannot set contact
    s, b = req("POST", "/rpc/set_listing_contact",
               {"listing_id": clid, "phone": "+996555000000"}, token=buyer)
    chk("non-owner set_contact -> 400", s == 400, f"status={s}")

    # events logged: 2 by buyer (number_revealed + whatsapp_clicked), seller self-view none
    req("POST", "/rpc/reveal_contact", {"listing_id": clid}, token=tok)   # seller views own
    c = sqlite3.connect(db)
    rows = c.execute("SELECT kind, actor_id, seller_id FROM contact_event WHERE listing_id=? ORDER BY created_at", (clid,)).fetchall()
    c.close()
    kinds = [r[0] for r in rows]
    chk("event: number_revealed logged", "number_revealed" in kinds, str(kinds))
    chk("event: whatsapp_clicked logged", "whatsapp_clicked" in kinds, str(kinds))
    chk("seller self-view not logged (2 events)", len(rows) == 2, str(rows))

    # ---- A1.6 part 2: chat (conversations + messages, VIA-scoped) ----
    def login_tok(creds):
        return (req("POST", "/auth/login", {"email": creds[0], "password": creds[1]})[1] or {}).get("token")
    seller, stranger = login_tok(SELLER), login_tok(STRANGER)
    chk("login seller + stranger", bool(seller) and bool(stranger))

    # seller (a normal user, so VIA scoping applies) posts a chat-enabled listing
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Chat Car", "price": 7000, "city_id": "ci-bishkek",
                "allow_chat": 1, "attributes": {"make": "Toyota", "year": 2017}}, token=seller)
    chat_lid = (b or {}).get("row", {}).get("id")
    seller_id = (b or {}).get("row", {}).get("seller_id")
    chk("seller posts chat listing -> 201", s == 201, f"status={s}")

    # buyer starts a conversation
    s, b = req("POST", "/rpc/start_conversation", {"listing_id": chat_lid}, token=buyer)
    res = (b or {}).get("result") or {}
    conv = res.get("conversation_id")
    chk("start_conversation -> 200", s == 200 and bool(conv), f"status={s} {b}")
    chk("conversation is new", res.get("existing") is False, str(res))

    # dedup: second start returns the same conversation
    s, b = req("POST", "/rpc/start_conversation", {"listing_id": chat_lid}, token=buyer)
    res = (b or {}).get("result") or {}
    chk("start dedups", res.get("conversation_id") == conv and res.get("existing") is True, str(res))

    # can't chat with yourself
    s, b = req("POST", "/rpc/start_conversation", {"listing_id": chat_lid}, token=seller)
    chk("seller self-chat -> 400", s == 400, f"status={s}")

    # buyer + seller exchange messages via /api/message (realtime fires on create)
    s, b = req("POST", "/api/message", {"conversation_id": conv, "body": "Hello, still available?"}, token=buyer)
    buyer_id = (b or {}).get("row", {}).get("sender_id")
    chk("buyer message -> 201", s == 201, f"status={s} {b}")
    chk("sender_id set server-side", bool(buyer_id) and buyer_id != seller_id, str(buyer_id))
    s, b = req("POST", "/api/message", {"conversation_id": conv, "body": "Yes!"}, token=seller)
    chk("seller message -> 201", s == 201, f"status={s}")

    # a non-participant cannot post into the conversation (before() membership check)
    s, b = req("POST", "/api/message", {"conversation_id": conv, "body": "intruding"}, token=stranger)
    chk("non-participant message -> 400", s == 400, f"status={s}")
    # empty body rejected
    s, b = req("POST", "/api/message", {"conversation_id": conv, "body": "   "}, token=buyer)
    chk("empty body -> 400", s == 400, f"status={s}")

    # VIA read scoping (same membership SQL as realtime subscribe): a participant
    # sees the messages; a non-participant sees NONE even querying the conv id.
    wq = "/api/message?order=created_at&where=" + urllib.parse.quote(json.dumps({"conversation_id": {"eq": conv}}))
    s, b = req("GET", wq, token=buyer)
    chk("participant reads 2 messages", s == 200 and (b or {}).get("count") == 2, f"status={s} {b}")
    s, b = req("GET", wq, token=stranger)
    chk("non-participant reads 0 (VIA scoped)", s == 200 and (b or {}).get("count") == 0, f"status={s} {(b or {}).get('count')}")

    # inbox: the conversation surfaces for buyer with last message + counterpart
    s, b = req("POST", "/rpc/inbox", {}, token=buyer)
    convs = ((b or {}).get("result") or {}).get("conversations") or []
    mine = [c for c in convs if c.get("id") == conv]
    chk("inbox lists the conversation", len(mine) == 1, str([c.get("id") for c in convs]))
    chk("inbox last_message preview", mine and mine[0].get("last_message") == "Yes!", str(mine[:1]))
    chk("inbox counterpart = seller", mine and mine[0].get("counterpart") == seller_id, str(mine[:1]))
    chk("inbox listing title", mine and mine[0].get("listing_title") == "Chat Car", str(mine[:1]))

    # a chat-disabled listing can't start a conversation
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "No Chat Car", "price": 1, "city_id": "ci-bishkek",
                "allow_chat": 0, "attributes": {"make": "Honda", "year": 2010}}, token=seller)
    nochat = (b or {}).get("row", {}).get("id")
    s, b = req("POST", "/rpc/start_conversation", {"listing_id": nochat}, token=buyer)
    chk("start on chat-disabled -> 400", s == 400, f"status={s}")

    # ---- A1.7 favorites ----
    s, b = req("POST", "/rpc/favorite", {"listing_id": clid}, token=buyer)
    chk("favorite -> 200", s == 200 and ((b or {}).get("result") or {}).get("favorited") is True, str(b))

    s, b = req("POST", "/rpc/favorites", {}, token=buyer)
    favs = ((b or {}).get("result") or {}).get("favorites") or []
    chk("favorites list includes it", any(f.get("id") == clid for f in favs), str([f.get("id") for f in favs]))

    # listing detail reflects the viewer's save state + social-proof count
    s, b = req("POST", "/rpc/listing", {"id": clid}, token=buyer)
    res = (b or {}).get("result") or {}
    chk("detail favorited=true (buyer)", res.get("favorited") is True, str(res.get("favorited")))
    chk("detail favorite_count=1", res.get("favorite_count") == 1, str(res.get("favorite_count")))
    s, b = req("POST", "/rpc/listing", {"id": clid})            # anon
    res = (b or {}).get("result") or {}
    chk("detail favorited=false (anon)", res.get("favorited") is False, str(res.get("favorited")))
    chk("anon still sees count=1", res.get("favorite_count") == 1, str(res.get("favorite_count")))

    # idempotent
    req("POST", "/rpc/favorite", {"listing_id": clid}, token=buyer)
    s, b = req("POST", "/rpc/listing", {"id": clid}, token=buyer)
    chk("favorite idempotent (count still 1)", ((b or {}).get("result") or {}).get("favorite_count") == 1, str(b))

    # unfavorite
    s, b = req("POST", "/rpc/unfavorite", {"listing_id": clid}, token=buyer)
    chk("unfavorite -> 200", s == 200 and ((b or {}).get("result") or {}).get("favorited") is False, str(b))
    s, b = req("POST", "/rpc/favorites", {}, token=buyer)
    favs = ((b or {}).get("result") or {}).get("favorites") or []
    chk("removed from favorites", not any(f.get("id") == clid for f in favs), str([f.get("id") for f in favs]))
    s, b = req("POST", "/rpc/listing", {"id": clid}, token=buyer)
    chk("count back to 0", ((b or {}).get("result") or {}).get("favorite_count") == 0, str(b))

    # guards
    s, b = req("POST", "/rpc/favorite", {"listing_id": "no-such-listing"}, token=buyer)
    chk("favorite missing listing -> 400", s == 400, f"status={s}")
    s, b = req("POST", "/rpc/favorite", {"listing_id": clid})   # anon
    chk("favorite anon -> 401 (engine _rpc gate)", s == 401, f"status={s}")

    # ---- A1.8 auth-gate audit: ownership on listing edit/delete ----
    # seller posts a listing; a DIFFERENT user must not be able to edit or delete it
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Owned by seller", "price": 3000,
                "city_id": "ci-bishkek", "attributes": {"make": "Toyota", "year": 2014}}, token=seller)
    own = (b or {}).get("row", {})
    own_id = own.get("id")
    chk("seller posts -> 201", s == 201, f"status={s}")
    chk("create forces seller_id (anti-spoof)", own.get("seller_id") == seller_id, str(own.get("seller_id")))

    # forged seller_id on create is overwritten by the hook
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Spoof attempt", "price": 1, "city_id": "ci-bishkek",
                "seller_id": buyer_id, "attributes": {"make": "Honda", "year": 2014}}, token=seller)
    chk("forged seller_id ignored", (b or {}).get("row", {}).get("seller_id") == seller_id, str((b or {}).get("row", {}).get("seller_id")))

    # a non-owner cannot edit (owner_column scopes the UPDATE to zero rows -> 404)
    s, b = req("PATCH", f"/api/listings/{own_id}", {"title": "HACKED"}, token=buyer)
    chk("non-owner edit -> 404 (owner-scoped)", s == 404, f"status={s}")
    # ...and the title is unchanged
    s, b = req("POST", "/rpc/listing", {"id": own_id})
    chk("title unchanged after failed edit", ((b or {}).get("result") or {}).get("listing", {}).get("title") == "Owned by seller", str(b)[:80])
    # a non-owner cannot delete
    s, b = req("DELETE", f"/api/listings/{own_id}", token=buyer)
    chk("non-owner delete -> 404", s == 404, f"status={s}")
    s, b = req("POST", "/rpc/listing", {"id": own_id})
    chk("listing survives non-owner delete", s == 200, f"status={s}")

    # the owner CAN edit + delete their own
    s, b = req("PATCH", f"/api/listings/{own_id}", {"title": "Updated by owner"}, token=seller)
    chk("owner edit -> 200", s == 200, f"status={s}")
    s, b = req("DELETE", f"/api/listings/{own_id}", token=seller)
    chk("owner delete -> 200", s == 200, f"status={s}")

    # anon cannot create / edit / delete (write path requires auth)
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "anon post", "attributes": {"make": "Toyota", "year": 2014}})
    chk("anon create -> 401", s == 401, f"status={s}")

    # ---- Phase 2 wiring: EventSink + JobQueue exercised through the app ----
    def rows_of(sql, args=()):
        c = sqlite3.connect(db)
        try: return c.execute(sql, args).fetchall()
        except Exception: return None
        finally: c.close()

    # a listing view emits a listing_viewed event (collected for all viewers)
    req("POST", "/rpc/listing", {"id": one.get("id")})          # one = a moto listing seeded earlier
    evs = rows_of("SELECT subject_id FROM event WHERE type='listing_viewed'")
    chk("EventSink: listing view emits event",
        evs is not None and any(r[0] == one.get("id") for r in evs), str(evs)[:100] if evs is not None else "no event table")
    facet_search(q="toyota", category="cat-cars")
    chk("EventSink: search emits event", len(rows_of("SELECT 1 FROM event WHERE type='search'") or []) >= 1, "")
    # actor + intent captured: a category browse (NO query) by a known user must emit a
    # search event stamped with that user's id + the category in props. (Previously a
    # no-query browse emitted nothing, and search events had no actor — both fixed.)
    req("POST", "/rpc/search", {"category": "cat-cars"}, token=seller)
    ev = rows_of("SELECT actor_id, props FROM event WHERE type='search' ORDER BY id DESC LIMIT 1")
    chk("EventSink: search event captures actor",
        bool(ev) and ev[0][0] == seller_id, str(ev)[:120] if ev else "no event")
    chk("EventSink: category browse emits + props carries category",
        bool(ev) and bool(ev[0][1]) and "cat-cars" in ev[0][1], str(ev)[:120] if ev else "no event")
    # escaper: a query with JSON-hostile chars (embedded quote + backslash) must still
    # produce VALID, parseable props with the query round-tripping intact.
    nasty = 'sony "x\\y" z'
    req("POST", "/rpc/search", {"q": nasty, "category": "cat-cars"}, token=seller)
    ev = rows_of("SELECT props FROM event WHERE type='search' ORDER BY id DESC LIMIT 1")
    try:
        parsed = json.loads(ev[0][0]) if ev else {}
        json_safe = parsed.get("q") == nasty and parsed.get("category") == "cat-cars"
    except Exception:
        json_safe = False
    chk("EventSink: search props JSON-safe w/ quote+backslash", json_safe, str(ev)[:160] if ev else "no event")

    # ---- rpc track: client behavioral event ingestion ----
    lsub = one.get("id")
    # valid batch (authed): accepted + emitted, actor SERVER-stamped (client 'actor' ignored)
    s, b = req("POST", "/rpc/track", {"events": [
        {"type": "impression",   "subject": lsub, "props": '{"pos":1,"ctx":"feed"}', "actor": "SPOOFED"},
        {"type": "result_click", "subject": lsub, "props": '{"pos":1}'},
    ]}, token=seller)
    r = (b or {}).get("result") or {}
    chk("track: valid batch accepted (2/0)", r.get("accepted") == 2 and r.get("dropped") == 0, str(r))
    tev = rows_of("SELECT actor_id, type FROM event WHERE type IN ('impression','result_click') ORDER BY id DESC LIMIT 2")
    chk("track: emitted w/ server actor (spoof ignored)",
        bool(tev) and len(tev) == 2 and all(row[0] == seller_id for row in tev), str(tev)[:140])
    # disallowed types dropped (can't forge server-authoritative events like 'contact')
    s, b = req("POST", "/rpc/track", {"events": [
        {"type": "contact", "subject": lsub}, {"type": "evil", "subject": lsub},
        {"type": "dwell",   "subject": lsub, "props": '{"ms":1200}'},
    ]}, token=seller)
    r = (b or {}).get("result") or {}
    chk("track: disallowed types dropped, allowed kept (1/2)", r.get("accepted") == 1 and r.get("dropped") == 2, str(r))
    # invalid props string -> coerced to '{}'
    req("POST", "/rpc/track", {"events": [{"type": "card_view", "subject": lsub, "props": "not json"}]}, token=seller)
    tev = rows_of("SELECT props FROM event WHERE type='card_view' ORDER BY id DESC LIMIT 1")
    chk("track: invalid props coerced to {}", bool(tev) and tev[0][0] == "{}", str(tev)[:100])
    # batch cap: 25 events -> 20 accepted, 5 dropped
    s, b = req("POST", "/rpc/track", {"events": [{"type": "impression", "subject": lsub} for _ in range(25)]}, token=seller)
    r = (b or {}).get("result") or {}
    chk("track: batch capped at 20 (20/5)", r.get("accepted") == 20 and r.get("dropped") == 5, str(r))
    # anon allowed: no token -> accepted, actor stamped empty
    s, b = req("POST", "/rpc/track", {"events": [{"type": "search_view", "subject": lsub, "props": '{"q":"x"}'}]})
    r = (b or {}).get("result") or {}
    tev = rows_of("SELECT actor_id FROM event WHERE type='search_view' ORDER BY id DESC LIMIT 1")
    chk("track: anon accepted, actor empty",
        r.get("accepted") == 1 and bool(tev) and (tev[0][0] == "" or tev[0][0] is None), str(r) + " " + str(tev)[:60])

    # ---- rollup_interest: event stream -> per-user category affinity ----
    s, b = req("POST", "/rpc/enqueue_job", {"type": "rollup_interest"}, token=tok)   # admin
    chk("rollup: enqueue -> id", s == 200 and ((b or {}).get("result") or {}).get("id", 0) > 0, str(b))
    req("POST", "/jobs/run", {}, token=tok)
    ui = rows_of("SELECT category_id, score FROM user_interest WHERE user_id = ? ORDER BY score DESC", (seller_id,))
    chk("rollup: seller has category affinity", bool(ui) and any(row[1] > 0 for row in ui), str(ui)[:160])
    chk("rollup: cat-cars affinity present (from seller searches)",
        bool(ui) and any(row[0] == "cat-cars" and row[1] > 0 for row in ui), str(ui)[:160])
    # cursor persisted + advanced (a second run with no new events doesn't move it)
    cur1 = rows_of("SELECT cursor FROM rollup_state WHERE name='user_interest'")
    req("POST", "/rpc/enqueue_job", {"type": "rollup_interest"}, token=tok)
    req("POST", "/jobs/run", {}, token=tok)
    cur2 = rows_of("SELECT cursor FROM rollup_state WHERE name='user_interest'")
    chk("rollup: cursor persisted, stable w/ no new events",
        bool(cur1) and bool(cur2) and cur1[0][0] > 0 and cur2[0][0] == cur1[0][0], f"{cur1}->{cur2}")
    # track scan ceiling: a 150-event batch is bounded at TRACK_SCAN_MAX(100) -> 20 accepted,
    # 80 dropped (positions 21-100); the rest aren't even scanned (the O(n^2) DoS guard).
    s, b = req("POST", "/rpc/track", {"events": [{"type": "impression", "subject": lsub} for _ in range(150)]}, token=seller)
    r = (b or {}).get("result") or {}
    chk("track: scan ceiling bounds huge batch (20/80)", r.get("accepted") == 20 and r.get("dropped") == 80, str(r))
    # rollup category canonicalization: a search by SLUG keys the canonical id; a junk
    # category creates NO row (blocks arbitrary user_interest injection via props.category).
    req("POST", "/rpc/search", {"category": "cars"}, token=seller)             # slug, not the id
    req("POST", "/rpc/search", {"category": "zzz-nonexistent"}, token=seller)  # junk
    req("POST", "/rpc/enqueue_job", {"type": "rollup_interest"}, token=tok)
    req("POST", "/jobs/run", {}, token=tok)
    cats = [row[0] for row in (rows_of("SELECT category_id FROM user_interest WHERE user_id = ?", (seller_id,)) or [])]
    chk("rollup: slug search canonicalized to id (cat-cars)", "cat-cars" in cats, str(cats))
    chk("rollup: junk category not keyed (no injection)",
        "zzz-nonexistent" not in cats and "cars" not in cats, str(cats))

    # ---- rpc feed: ranked home feed (non-personalized + personalized) ----
    def feed_of(token=None, **kw):
        s, b = req("POST", "/rpc/feed", kw, token=token)
        return s, (b or {}).get("result") or {}
    s, anon_feed = feed_of(limit=50)
    chk("feed: anon returns ranked active listings",
        s == 200 and (anon_feed.get("total") or 0) > 0 and len(anon_feed.get("results") or []) > 0, str(anon_feed.get("total")))
    chk("feed: anon is non-personalized", anon_feed.get("personalized") is False, str(anon_feed.get("personalized")))
    chk("feed: cards carry feed_score + card shape",
        bool(anon_feed.get("results")) and all(("feed_score" in r and "title" in r and "saved_count" in r) for r in anon_feed["results"]),
        str((anon_feed.get("results") or [{}])[0])[:140])
    s, pers_feed = feed_of(token=seller, limit=50)
    chk("feed: authed is personalized", pers_feed.get("personalized") is True, str(pers_feed.get("personalized")))
    # affinity effect: the seller has cat-moto affinity (built by the rollup); a cat-moto
    # listing must score STRICTLY higher in the seller's feed than the anon feed (aff>0 vs 0).
    def smap(res): return {r["id"]: (r.get("feed_score") or 0, r.get("category_id")) for r in (res.get("results") or [])}
    am, pm = smap(anon_feed), smap(pers_feed)
    moto_boosted = any(pm[i][0] > am[i][0] for i in pm if i in am and pm[i][1] == "cat-moto")
    chk("feed: personalization boosts the affine category (cat-moto)", moto_boosted,
        f"moto listings: {[i for i in pm if pm[i][1]=='cat-moto']}")

    # JobQueue: an expire sweep flips a past-expiry active listing to 'expired'
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Will expire", "city_id": "ci-bishkek",
                "attributes": {"make": "Toyota", "year": 2015}}, token=seller)
    exp_id = (b or {}).get("row", {}).get("id")
    req("PATCH", f"/api/listings/{exp_id}", {"expires_at": "2000-01-01T00:00:00Z"}, token=seller)
    s, b = req("POST", "/rpc/enqueue_job", {"type": "expire_listings"}, token=tok)   # admin
    chk("JobQueue: enqueue_job -> id", s == 200 and ((b or {}).get("result") or {}).get("id", 0) > 0, str(b))
    s, b = req("POST", "/jobs/run", {}, token=tok)                                   # admin
    chk("JobQueue: /jobs/run processes >=1", s == 200 and (b or {}).get("processed", 0) >= 1, str(b))
    st = rows_of("SELECT status FROM listings WHERE id=?", (exp_id,))
    chk("JobQueue: job expired the listing", st and st[0][0] == "expired", str(st))
    # one-shot job consumed
    chk("JobQueue: one-shot job consumed", (rows_of("SELECT count(*) FROM job")[0][0]) == 0, "")
    # non-admin cannot run jobs
    s, _ = req("POST", "/jobs/run", {}, token=seller)
    chk("JobQueue: non-admin /jobs/run -> 403", s == 403, f"status={s}")

    # ---- A2.2 in-app notification feed ----
    # the chat earlier notified the buyer (seller replied) and the seller (buyer wrote)
    s, b = req("POST", "/rpc/unread_count", {}, token=buyer)
    chk("notif: buyer has unread", ((b or {}).get("result") or {}).get("unread", 0) >= 1, str(b))
    s, b = req("POST", "/rpc/notifications", {}, token=buyer)
    notes = ((b or {}).get("result") or {}).get("notifications") or []
    chk("notif: a 'message' notification", any(n.get("type") == "message" for n in notes), str([n.get("type") for n in notes]))
    chk("notif: carries deep-link subject", any(n.get("subject_id") for n in notes), str(notes[:1]))

    # the expire job (run above) notified the seller about their expired listing
    s, b = req("POST", "/rpc/notifications", {"unread_only": True}, token=seller)
    snotes = ((b or {}).get("result") or {}).get("notifications") or []
    chk("notif: seller got listing_expired", any(n.get("type") == "listing_expired" for n in snotes),
        str([n.get("type") for n in snotes]))

    # mark_read clears unread; owner-scoped reads
    s, b = req("POST", "/rpc/mark_read", {}, token=buyer)        # all mine
    chk("notif: mark_read -> unread 0", ((b or {}).get("result") or {}).get("unread") == 0, str(b))
    s, b = req("GET", "/api/notification", token=seller)
    rows = (b or {}).get("rows") or []
    chk("notif: /api owner-scoped", len(rows) > 0 and all(r.get("user_id") == seller_id for r in rows), f"n={len(rows)}")
    s, b = req("GET", "/api/notification")                       # anon
    chk("notif: anon /api denied -> 401", s == 401, f"status={s}")

    # ---- A2.1 lifecycle: renew + recurring-job dedup ----
    s, b = req("POST", "/rpc/renew_listing", {"listing_id": exp_id}, token=seller)   # exp_id is 'expired'
    chk("A2.1 renew_listing -> 200", s == 200 and bool(((b or {}).get("result") or {}).get("expires_at")), str(b))
    st = rows_of("SELECT status FROM listings WHERE id=?", (exp_id,))
    chk("A2.1 renew reactivated listing", st and st[0][0] == "active", str(st))
    s, _ = req("POST", "/rpc/renew_listing", {"listing_id": exp_id}, token=buyer)     # non-owner
    chk("A2.1 non-owner renew -> 400", s == 400, f"status={s}")
    s, b = req("POST", "/rpc/enqueue_job", {"type": "dedup_test", "repeat_every": 3600}, token=tok)
    id1 = ((b or {}).get("result") or {}).get("id")
    s, b = req("POST", "/rpc/enqueue_job", {"type": "dedup_test", "repeat_every": 3600}, token=tok)
    res = (b or {}).get("result") or {}
    chk("A2.1 recurring job dedups by type", res.get("id") == id1 and res.get("existing") is True, str(res))

    # ---- A2.4 event instrumentation: favorite + contact emitted ----
    chk("A2.4 favorite event emitted", len(rows_of("SELECT 1 FROM event WHERE type='favorite'") or []) >= 1, "")
    chk("A2.4 contact event emitted", len(rows_of("SELECT 1 FROM event WHERE type='contact'") or []) >= 1, "")

    # ---- A2.3 saved searches + matcher → alert ----
    s, b = req("POST", "/rpc/save_search", {"q": "toyota", "category": "cat-cars", "name": "Toyotas"}, token=seller)
    ss_id = ((b or {}).get("result") or {}).get("id")
    chk("A2.3 save_search -> id", s == 200 and bool(ss_id), str(b))
    time.sleep(1.1)   # new listing's created_at must exceed last_run_at (1s ISO precision)
    s, b = req("POST", "/api/listings",
               {"category_id": "cat-cars", "title": "Toyota Land Cruiser fresh", "city_id": "ci-bishkek",
                "attributes": {"make": "Toyota", "year": 2020}}, token=buyer)
    new_lid = (b or {}).get("row", {}).get("id")
    chk("A2.3 new matching listing", s == 201, f"status={s}")
    req("POST", "/rpc/enqueue_job", {"type": "match_saved_searches"}, token=tok)
    req("POST", "/jobs/run", {}, token=tok)
    s, b = req("POST", "/rpc/notifications", {"unread_only": True}, token=seller)
    ssn = ((b or {}).get("result") or {}).get("notifications") or []
    chk("A2.3 saved-search alert fired",
        any(n.get("type") == "saved_search" and n.get("subject_id") == new_lid for n in ssn),
        str([(n.get("type"), n.get("subject_id")) for n in ssn]))
    s, b = req("POST", "/rpc/saved_searches", {}, token=seller)
    chk("A2.3 saved_searches lists it",
        any(x.get("id") == ss_id for x in (((b or {}).get("result") or {}).get("searches") or [])), str(b)[:80])
    req("POST", "/rpc/delete_saved_search", {"id": ss_id}, token=seller)
    s, b = req("POST", "/rpc/saved_searches", {}, token=seller)
    chk("A2.3 delete_saved_search removes it",
        not any(x.get("id") == ss_id for x in (((b or {}).get("result") or {}).get("searches") or [])), "")

    # ---- review hardening (security-review follow-ups) ----
    base_ok = {"category_id": "cat-cars", "title": "Hardening", "city_id": "ci-bishkek",
               "attributes": {"make": "Toyota", "year": 2016}}
    # #5 price: negative / fractional rejected
    s, _ = req("POST", "/api/listings", dict(base_ok, price=-100), token=seller)
    chk("negative price -> 400", s == 400, f"status={s}")
    s, _ = req("POST", "/api/listings", dict(base_ok, price=10.5), token=seller)
    chk("fractional price -> 400", s == 400, f"status={s}")
    # is_free: price 0 now rejected; a free listing stores is_free=1, nulls price, and is searchable
    s, _ = req("POST", "/api/listings", dict(base_ok, price=0), token=seller)
    chk("price 0 -> 400 (use is_free)", s == 400, f"status={s}")
    s, b = req("POST", "/api/listings", dict(base_ok, title="Free thing", is_free=True, price=500), token=seller)
    frow = (b or {}).get("row", {})
    chk("free listing -> 201", s == 201, f"status={s}")
    chk("is_free stored as 1 + price nulled", frow.get("is_free") == 1 and frow.get("price") is None, str(frow))
    s, b = req("POST", "/rpc/search", {"free": True, "category": "cat-cars"})
    r = (b or {}).get("result") or {}
    chk("search free filter -> only free rows",
        r.get("total", 0) >= 1 and all(x.get("is_free") == 1 for x in (r.get("results") or [])), str(r.get("total")))
    s, b = req("POST", "/rpc/search", {"category": "cat-cars"})
    r = (b or {}).get("result") or {}
    chk("search reports free_count", isinstance(r.get("free_count"), int) and r.get("free_count") >= 1, str(r.get("free_count")))
    # is_free UPDATE integrity (review fix): flip priced->free nulls price; set price->clears is_free
    def listing_row(lid_):
        _s, _b = req("POST", "/rpc/listing", {"id": lid_}, token=seller)
        return ((_b or {}).get("result") or {}).get("listing") or {}
    s, b = req("POST", "/api/listings", dict(base_ok, title="Priced then free", price=7000), token=seller)
    plid = (b or {}).get("row", {}).get("id")
    chk("priced listing -> 201", s == 201 and bool(plid), f"status={s}")
    req("PATCH", f"/api/listings/{plid}", {"is_free": True}, token=seller)
    g = listing_row(plid)
    chk("PATCH to free nulls the price", g.get("is_free") == 1 and g.get("price") is None, str((g.get("is_free"), g.get("price"))))
    req("PATCH", f"/api/listings/{plid}", {"price": 3000}, token=seller)
    g = listing_row(plid)
    chk("PATCH price on free clears is_free", g.get("is_free") == 0 and g.get("price") == 3000, str((g.get("is_free"), g.get("price"))))
    # #6 created_at is server-owned (a forged far-future value is ignored)
    s, b = req("POST", "/api/listings", dict(base_ok, created_at="2099-01-01T00:00:00Z"), token=seller)
    hid = (b or {}).get("row", {}).get("id")
    chk("forged created_at ignored", (b or {}).get("row", {}).get("created_at", "").startswith("202"),
        str((b or {}).get("row", {}).get("created_at")))
    # #3 updating attributes without category_id is rejected (no silent bypass)
    s, _ = req("PATCH", f"/api/listings/{hid}", {"attributes": {"make": "NotAMake", "year": 2016}}, token=seller)
    chk("update attrs w/o category_id -> 400", s == 400, f"status={s}")
    # #3b symmetric: changing category WITHOUT attributes is rejected (re-review
    # follow-up — would otherwise leave stale attributes unvalidated + pollute facets)
    s, _ = req("PATCH", f"/api/listings/{hid}", {"category_id": "cat-apartments"}, token=seller)
    chk("change category w/o attributes -> 400", s == 400, f"status={s}")
    # ...and with category_id it still validates (bad enum rejected)
    s, _ = req("PATCH", f"/api/listings/{hid}",
               {"category_id": "cat-cars", "attributes": {"make": "NotAMake", "year": 2016}}, token=seller)
    chk("update attrs bad enum -> 400", s == 400, f"status={s}")
    # a valid category+attributes change succeeds
    s, _ = req("PATCH", f"/api/listings/{hid}",
               {"category_id": "cat-apartments", "attributes": {"deal": "Аренда долгосрочная", "rooms": 2}}, token=seller)
    chk("valid category+attrs change -> 200", s == 200, f"status={s}")
    # a PATCH touching neither category nor attributes is unaffected
    s, _ = req("PATCH", f"/api/listings/{hid}", {"title": "Just a title edit"}, token=seller)
    chk("title-only PATCH -> 200", s == 200, f"status={s}")

    # #1 /api/listings is owner-scoped: a user sees only their OWN listings
    s, b = req("GET", "/api/listings", token=seller)
    rows = (b or {}).get("rows") or []
    chk("seller /api list -> own only", s == 200 and all(r.get("seller_id") == seller_id for r in rows) and len(rows) > 0,
        f"status={s} n={len(rows)}")
    s, b = req("GET", "/api/listings", token=buyer)
    rows = (b or {}).get("rows") or []
    chk("buyer /api list excludes seller's", all(r.get("seller_id") != seller_id for r in rows), f"n={len(rows)}")
    s, b = req("GET", f"/api/listings/{hid}", token=buyer)   # buyer fetching seller's listing
    chk("buyer get seller's listing -> 404 (owner-scoped)", s == 404, f"status={s}")

if __name__ == "__main__":
    sys.exit(main())
