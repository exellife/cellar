#!/usr/bin/env python3
"""Regenerate the classifieds taxonomy seed labels from the bilingual specs.

Source of truth = taxonomy/*.md (one file per top category), written `English [KY]`
— ky is the [bracket] when present, else the English word (bare tokens like
SUV/Toyota/32 are the same in both).

Model (consistent across all three levels):
  - category           : name (plain English fallback)  + labels {"ky":…}
  - category_attribute : label (plain English fallback)  + labels {"en":…,"ky":…}
  - enum option        : code (canonical)                + labels {"en":…,"ky":…}
English + codes are never touched; only the display labels are (re)generated.

This rewrites seed.sql ONLY. Migrations are hand-authored history (immutable) —
see migrations/0003_ky_labels.sql (ky) and 0004_attr_labels.sql (label→labels
split). Re-running this never touches them, so a shipped migration can't drift.

Options are matched to the DB by their ENGLISH LABEL, not by position — the specs
are NOT reliably in cellar order (e.g. transmission). Any option that can't be
matched is a hard error (we refuse to mislabel a code).

Usage:  python3 gen_ky_labels.py [--check]   (--check = validate + review, write nothing)
"""
import json, re, sqlite3, subprocess, sys, tempfile, os, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SPEC_DIR = HERE / "taxonomy"
SEED = HERE / "seed.sql"
SCHEMA = HERE / "schema.sql"
SPECS = ["transport", "real-estate", "electronics", "home-garden",
         "personal-items", "animals"]          # jobs/services are stubs — skipped

def norm(s):
    """Match key for an English option label: casefold, unify dashes, squash space."""
    return re.sub(r"\s+", " ", s.replace("–", "-").replace("—", "-")
                  .replace("−", "-")).strip().casefold()

def split_en_ky(tok):
    """'Automatic [Автомат]' -> ('Automatic','Автомат');  'SUV' -> ('SUV','SUV')."""
    m = re.match(r"^(.*?)\s*(?:\[(.+?)\])?\s*$", tok.strip())
    en = m.group(1).strip()
    ky = (m.group(2) or en).strip()
    return en, ky

# ── parse the specs ─────────────────────────────────────────────────────────
cat_ky = {}                     # cat_id -> ky
attr = {}                       # (cat_id, key) -> {"en","ky","opts":[(en,ky)]}
for name in SPECS:
    lines = (SPEC_DIR / f"{name}.md").read_text(encoding="utf-8").splitlines()
    mode = None; cur_cat = None
    for ln in lines:
        m = re.match(r"^#\s+(.*?)\s*(?:\[(.+?)\])?\s*·\s*`([\w-]+)`", ln)   # top heading
        if m:
            cat_ky[m.group(3)] = (m.group(2) or m.group(1)).strip(); continue
        if re.match(r"^##\s+Subcategories", ln): mode = "sub"; continue
        m = re.match(r"^##\s+.*?·\s*`([\w-]+)`", ln)                        # section = a cat
        if m: mode = "attr"; cur_cat = m.group(1); continue
        if mode == "sub":
            m = re.match(r"^-\s+\*\*(.+?)\*\*\s*(?:\[(.+?)\])?\s*·\s*`([\w-]+)`", ln)
            if m: cat_ky[m.group(3)] = (m.group(2) or m.group(1)).strip()
        elif mode == "attr":
            m = re.match(r"^-\s+\*\*(.+?)\*\*\s*(?:\[(.+?)\])?\s*`(\w+)`\s*\((.+?)\)\s*(?::\s*(.+))?$", ln)
            if not m: continue
            en, ky, key = m.group(1).strip(), (m.group(2) or m.group(1)).strip(), m.group(3)
            opts = [split_en_ky(t) for t in m.group(5).split("·")] if m.group(5) else []
            attr[(cur_cat, key)] = {"en": en, "ky": ky, "opts": opts}

# ── read the current rows (structural fields) from a throwaway DB ────────────
tmp = tempfile.mkdtemp(); db = os.path.join(tmp, "t.db")
subprocess.run(f"sqlite3 {db} < {SCHEMA}", shell=True, check=True)
subprocess.run(f"sqlite3 {db} < {SEED}", shell=True, check=True)
con = sqlite3.connect(db)
cats = con.execute("SELECT id, name, labels FROM category").fetchall()
ATTR_COLS = "id, category_id, key, label, labels, type, required, filterable, unit, options, depends_on, sort_order"
attrs = con.execute(f"SELECT {ATTR_COLS} FROM category_attribute ORDER BY rowid").fetchall()
con.close()

errors, notes = [], []
new_cat_labels = {}   # cat_id -> json str
new_attr_rows = []    # ordered list of dicts for the regenerated INSERT block

def dump(o): return json.dumps(o, ensure_ascii=False)

def as_obj(s):
    try:
        v = json.loads(s); return v if isinstance(v, dict) else None
    except Exception: return None

# categories (surgical — only labels.ky changes; name stays)
for cid, cname, clabels in cats:
    if cid not in cat_ky:
        notes.append(f"cat {cid}: not in specs — left as-is"); continue
    new_cat_labels[cid] = dump({"ky": cat_ky[cid]})

# attributes — compute label (plain en) + labels ({en,ky}) + options (with ky)
for (aid, cid, key, label_old, labels_old, atype, req, filt, unit, opts_old, dep, so) in attrs:
    spec = attr.get((cid, key))
    if spec:
        label_plain = spec["en"]
        labels_json = dump({"en": spec["en"], "ky": spec["ky"]})
    else:
        notes.append(f"attr {aid} ({cid}.{key}): not in specs — kept")
        obj = as_obj(label_old)                       # split a JSON-object label if present
        label_plain = (obj.get("en") if obj else label_old)
        labels_json = (dump(obj) if obj else labels_old)
    new_opts_json = opts_old
    if atype == "enum" and opts_old and spec:
        db_opts = json.loads(opts_old)
        spec_by_en = {norm(en): (en, ky) for en, ky in spec["opts"]}
        out = []
        for o in db_opts:
            code = o["code"]
            db_en = (o.get("labels") or {}).get("en") or code
            hit = spec_by_en.get(norm(db_en))
            if not hit:
                errors.append(f"attr {aid} ({cid}.{key}): option code={code!r} en={db_en!r} "
                              f"has no match in spec {[e for e,_ in spec['opts']]}"); out.append(o); continue
            en, ky = hit
            out.append({"code": code} if en == ky == code
                       else {"code": code, "labels": {"en": en, "ky": ky}})
        new_opts_json = dump(out)
    new_attr_rows.append(dict(id=aid, category_id=cid, key=key, label=label_plain,
                              labels=labels_json, type=atype, required=req, filterable=filt,
                              unit=unit, options=new_opts_json, depends_on=dep, sort_order=so))

# ── validate ────────────────────────────────────────────────────────────────
def bad(s): return bool(s) and bool(re.search(r"[ьъ]", s))
for cid, j in new_cat_labels.items():
    if bad(j): errors.append(f"cat {cid}: ky has ь/ъ -> {j}")
for r in new_attr_rows:
    for f in ("labels", "options"):
        if bad(r[f]): errors.append(f"attr {r['id']}: {f} has ь/ъ -> {r[f]}")
    if "'" in (r["label"] or "") or bad(r["label"]):
        errors.append(f"attr {r['id']}: label should be plain English -> {r['label']!r}")

print("=== categories updated:", len(new_cat_labels), " attributes:", len(new_attr_rows))
for n in notes: print("  note:", n)
if errors:
    print("\n!!! ERRORS (nothing written):")
    for e in errors: print("  -", e)
    sys.exit(1)
print("\n--- sample ---")
for cid in ("cat-transport", "cat-cars"):
    if cid in new_cat_labels: print(f"  {cid:14} {new_cat_labels[cid]}")
for r in new_attr_rows[:1] + [x for x in new_attr_rows if x["id"] == "ca-car-trans"]:
    print(f"  {r['id']:14} label={r['label']!r} labels={r['labels']}")
if "--check" in sys.argv:
    print("\n[--check] validated, wrote nothing."); sys.exit(0)

# ── rewrite seed.sql ────────────────────────────────────────────────────────
# All our JSON/label fields contain NO single-quote, so '[^']*' safely delimits one.
def sql(v):
    if v is None: return "NULL"
    if isinstance(v, int): return str(v)
    return "'" + v + "'"

text = SEED.read_text(encoding="utf-8")

# categories: surgical replace of the labels value (5th column: NULL or '...')
for cid, j in new_cat_labels.items():
    pat = r"(\('%s',\s*(?:NULL|'[^']*'),\s*'[^']*',\s*'[^']*',\s*)(?:NULL|'[^']*')" % re.escape(cid)
    new, n = re.subn(pat, lambda m: m.group(1) + "'" + j + "'", text, count=1)
    if n != 1: raise SystemExit(f"seed: category {cid} matched {n}x")
    text = new

# attributes: regenerate the whole INSERT block (adds the `labels` column, splits label).
cols = "id, category_id, key, label, labels, type, required, filterable, unit, options, depends_on, sort_order"
rows = []
for r in new_attr_rows:
    vals = [sql(r["id"]), sql(r["category_id"]), sql(r["key"]), sql(r["label"]), sql(r["labels"]),
            sql(r["type"]), sql(r["required"]), sql(r["filterable"]), sql(r["unit"]),
            sql(r["options"]), sql(r["depends_on"]), sql(r["sort_order"])]
    rows.append("  (" + ", ".join(vals) + ")")
block = f"INSERT INTO category_attribute ({cols}) VALUES\n" + ",\n".join(rows) + ";"
blk_pat = re.compile(r"INSERT INTO category_attribute \(.*?\) VALUES\n(?:.*\n)*?.*;", re.M)
new, n = blk_pat.subn(lambda _: block, text, count=1)
if n != 1: raise SystemExit(f"seed: category_attribute block matched {n}x")
SEED.write_text(new, encoding="utf-8")
print(f"\nwrote {SEED.relative_to(HERE.parent.parent)}  "
      f"({len(new_cat_labels)} cats surgical, {len(new_attr_rows)} attrs regenerated)")
