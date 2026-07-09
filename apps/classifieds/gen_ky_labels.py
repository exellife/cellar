#!/usr/bin/env python3
"""Regenerate Kyrgyz display labels (labels.ky) for the classifieds taxonomy.

Source of truth = the bilingual specs in ./taxonomy/*.md (one file per top
category), written `English [KY]` — ky is the [bracket] when present, else the
English word (bare tokens like SUV/Toyota/32 are the same in both).

This sets labels.ky on THREE things: categories, attribute labels, and enum
option values. English stays as-is (canonical); codes are never touched.

Outputs (both from one parse, so they can't drift):
  - rewrites  seed.sql             (canonical — used on a fresh provision)
  - writes    migrations/0003_ky_labels.sql  (UPDATEs — carries the same labels
              to already-provisioned DBs: local .run + prod, without wiping data)

Options are matched to the DB by their ENGLISH LABEL, not by position — the
specs are NOT reliably in cellar order (e.g. transmission). Any option that
can't be matched is a hard error (we refuse to mislabel a code).

Usage:  python3 gen_ky_labels.py [--check]
        --check  parse + validate + print the review, write nothing.
"""
import json, re, sqlite3, subprocess, sys, tempfile, os, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SPEC_DIR = HERE / "taxonomy"
SEED = HERE / "seed.sql"
SCHEMA = HERE / "schema.sql"
MIGRATION = HERE / "migrations" / "0003_ky_labels.sql"
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
attrs = con.execute("SELECT id, category_id, key, label, type, options FROM category_attribute").fetchall()
con.close()

errors, notes = [], []
new_cat_labels = {}   # cat_id -> json str
new_attr = {}         # attr_id -> (label_json, options_json_or_None)

def dump(o): return json.dumps(o, ensure_ascii=False)

# categories
for cid, cname, clabels in cats:
    if cid not in cat_ky:
        notes.append(f"cat {cid}: not in specs — left as-is"); continue
    new_cat_labels[cid] = dump({"ky": cat_ky[cid]})

# attributes + options
for aid, cid, key, label_json, atype, opts_json in attrs:
    spec = attr.get((cid, key))
    if not spec:
        notes.append(f"attr {aid} ({cid}.{key}): not in specs — left as-is"); continue
    new_label = dump({"en": spec["en"], "ky": spec["ky"]})
    new_opts_json = None
    if atype == "enum" and opts_json:
        db_opts = json.loads(opts_json)
        spec_by_en = {norm(en): (en, ky) for en, ky in spec["opts"]}
        out = []
        for o in db_opts:
            code = o["code"]
            db_en = (o.get("labels") or {}).get("en") or code
            hit = spec_by_en.get(norm(db_en))
            if not hit:
                errors.append(f"attr {aid} ({cid}.{key}): option code={code!r} en={db_en!r} "
                              f"has no match in spec options {[e for e,_ in spec['opts']]}")
                out.append(o); continue
            en, ky = hit
            if en == ky == code:
                out.append({"code": code})                       # bare — display falls back to code
            else:
                out.append({"code": code, "labels": {"en": en, "ky": ky}})
        new_opts_json = dump(out)
    new_attr[aid] = (new_label, new_opts_json)

# ── validate ────────────────────────────────────────────────────────────────
def has_soft_sign(s): return bool(re.search(r"[ьъ]", s))
for cid, j in new_cat_labels.items():
    if has_soft_sign(j): errors.append(f"cat {cid}: ky has ь/ъ -> {j}")
for aid, (lj, oj) in new_attr.items():
    if has_soft_sign(lj): errors.append(f"attr {aid}: label ky has ь/ъ -> {lj}")
    if oj and has_soft_sign(oj): errors.append(f"attr {aid}: option ky has ь/ъ -> {oj}")

print("=== categories updated:", len(new_cat_labels), " attributes updated:", len(new_attr))
for n in notes: print("  note:", n)
if errors:
    print("\n!!! ERRORS (nothing written):")
    for e in errors: print("  -", e)
    sys.exit(1)

# quick review sample
print("\n--- sample ---")
for cid in ("cat-transport", "cat-cars", "cat-trucks"):
    if cid in new_cat_labels: print(f"  {cid:16} {new_cat_labels[cid]}")
for aid in ("ca-car-trans", "ca-car-body"):
    if aid in new_attr: print(f"  {aid:16} {new_attr[aid][0]}\n{' '*19}{new_attr[aid][1]}")

if "--check" in sys.argv:
    print("\n[--check] validated, wrote nothing."); sys.exit(0)

# ── rewrite seed.sql (surgical: only the label/options JSON per row) ─────────
# All our JSON fields contain NO single-quote, so '[^']*' safely delimits a field.
text = SEED.read_text(encoding="utf-8")
def sub_once(pat, repl, s):
    new, n = re.subn(pat, lambda m: repl(m), s, count=1)
    if n != 1: raise SystemExit(f"seed rewrite: pattern matched {n}x (expected 1): {pat}")
    return new

for cid, j in new_cat_labels.items():
    # (id, parent, slug, name, LABELS, sort)  -> replace the 5th value (NULL or '...')
    pat = r"(\('%s',\s*(?:NULL|'[^']*'),\s*'[^']*',\s*'[^']*',\s*)(?:NULL|'[^']*')" % re.escape(cid)
    text = sub_once(pat, lambda m: m.group(1) + "'" + j + "'", text)

for aid, (lj, oj) in new_attr.items():
    # label = the 4th value, right after (id, category_id, key,
    pat = r"(\('%s',\s*'[^']*',\s*'[^']*',\s*)'[^']*'" % re.escape(aid)
    text = sub_once(pat, lambda m: m.group(1) + "'" + lj + "'", text)
    if oj is not None:
        # options = the first '[...]' token after the id on this row (label is '{...}',
        # depends_on is 'key' — neither is '[...]'). [^\n] keeps us on the one row.
        opat = r"(\('%s',[^\n]*?)'\[[^']*\]'" % re.escape(aid)
        text = sub_once(opat, lambda m: m.group(1) + "'" + oj + "'", text)
SEED.write_text(text, encoding="utf-8")

# ── emit the migration (data-preserving; idempotent — sets to the same values) ──
out = ["-- 0003_ky_labels.sql — Kyrgyz display labels for categories, attribute",
       "-- labels, and enum options. Generated by gen_ky_labels.py from taxonomy/*.md.",
       "-- English + codes unchanged; only labels.ky added/refreshed. Safe on live data",
       "-- (touches taxonomy label columns only; listing attribute VALUES are codes).", ""]
for cid, j in new_cat_labels.items():
    out.append("UPDATE category SET labels='%s' WHERE id='%s';" % (j, cid))
out.append("")
for aid, (lj, oj) in new_attr.items():
    if oj is not None:
        out.append("UPDATE category_attribute SET label='%s', options='%s' WHERE id='%s';" % (lj, oj, aid))
    else:
        out.append("UPDATE category_attribute SET label='%s' WHERE id='%s';" % (lj, aid))
out.append("")
MIGRATION.write_text("\n".join(out), encoding="utf-8")
print(f"\nwrote {SEED.relative_to(HERE.parent.parent)}")
print(f"wrote {MIGRATION.relative_to(HERE.parent.parent)}  ({len(new_cat_labels)+len(new_attr)} UPDATEs)")
