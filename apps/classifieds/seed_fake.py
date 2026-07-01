#!/usr/bin/env python3
"""Bulk-generate synthetic listings for scale / search / facet testing.

Loads N fake listings (+ a pool of seller users + the derived listing_facet
index + the FTS index) straight into a data.db via SQLite — NOT through the API
(the API tops out a few thousand writes/s; this does hundreds of thousands).
No images. Data is generated to MATCH the seeded category schema (real enum
options, sane numeric ranges) so search + faceted filtering are meaningful.

  usage: seed_fake.py <data.db> <count> [--users M] [--region kg]

Fast path: drop the FTS triggers, bulk-insert listings + facets set-based, then
`INSERT INTO listings_fts('rebuild')`, then recreate the triggers. foreign_keys
is off during load (referential integrity is guaranteed by construction).

Re-run safe-ish: appends more listings; facet insert is OR IGNORE.
"""
import sys, os, json, random, uuid, time, argparse

def iso(t): return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(t))

# per-key numeric ranges (heuristic); anything unmatched falls back to 1..1000
NUM_RANGES = {
    'year': (1995, 2024), 'mileage': (0, 350000), 'engine_cc': (49, 2500),
    'rooms': (1, 6), 'area': (18, 300), 'floor': (1, 16), 'total_floors': (3, 24),
    'land': (1, 50),
}
# per-category price range in whole soms
PRICE = {
    'cat-cars': (150_000, 6_000_000), 'cat-moto': (30_000, 800_000),
    'cat-apartments': (8_000, 25_000_000), 'cat-houses': (1_000_000, 40_000_000),
    'cat-phones': (3_000, 160_000), 'cat-computers': (5_000, 300_000),
}
MODELS = ['Pro', 'Lux', 'Sport', 'Classic', 'GT', 'X', 'Plus', 'Mini', 'Max', 'S', 'L']
ADJ = ['отличное', 'срочно', 'новое', 'недорого', 'торг', 'идеальное', 'выгодно']

def gen_attr(a, rnd):
    """A valid value for one category_attribute row `a` = (key,type,options)."""
    key, typ, opts = a
    if typ == 'enum':
        return rnd.choice(opts) if opts else None
    if typ == 'bool':
        return rnd.random() < 0.5
    if typ in ('int', 'number'):
        lo, hi = NUM_RANGES.get(key, (1, 1000))
        v = rnd.randint(lo, hi)
        return v if typ == 'int' else round(v + rnd.random(), 1)
    if typ == 'text':
        return rnd.choice(MODELS)
    return None

def title_for(cat_id, cat_name, attrs, rnd):
    if cat_id in ('cat-cars', 'cat-moto'):
        return f"{attrs.get('make','Авто')} {rnd.choice(MODELS)} {attrs.get('year','')}".strip()
    if cat_id in ('cat-apartments', 'cat-houses'):
        r = attrs.get('rooms', '')
        return f"{r}-комн {cat_name.lower()}".strip('- ')
    if cat_id == 'cat-phones':
        return f"{attrs.get('brand','Телефон')} {attrs.get('storage','')}ГБ".strip()
    return f"{cat_name} — {rnd.choice(ADJ)}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db'); ap.add_argument('count', type=int)
    ap.add_argument('--users', type=int, default=0)
    ap.add_argument('--region', default='kg')
    ap.add_argument('--seed', type=int, default=1234)
    ap.add_argument('--media', default='',
                    help='comma-separated media ids (must already exist via POST /media); '
                         '1-4 random are attached to each listing as photos')
    ap.add_argument('--owner', default='', help='assign a share of listings to this existing user id')
    ap.add_argument('--owner-share', type=float, default=0.1, dest='owner_share',
                    help='fraction of listings owned by --owner (default 0.1)')
    a = ap.parse_args()
    import sqlite3
    rnd = random.Random(a.seed)
    media_pool = [x for x in a.media.split(',') if x]

    con = sqlite3.connect(a.db)
    con.execute('PRAGMA foreign_keys=OFF')
    con.execute('PRAGMA journal_mode=WAL')
    con.execute('PRAGMA synchronous=OFF')
    cur = con.cursor()

    # postable (leaf) categories = those that have attributes
    cats = [r[0] for r in cur.execute(
        'SELECT id FROM category WHERE id IN (SELECT DISTINCT category_id FROM category_attribute)')]
    if not cats:
        print('no categories with attributes — load schema.sql + seed.sql first', file=sys.stderr); return 2
    cat_name = dict(cur.execute('SELECT id, name FROM category'))
    # attribute schema per category: list of (key, type, required, filterable, options)
    schema = {}
    for cid in cats:
        rows = cur.execute('SELECT key, type, required, filterable, options FROM category_attribute '
                           'WHERE category_id=? ORDER BY sort_order', (cid,)).fetchall()
        schema[cid] = [(k, t, req, filt, json.loads(o) if o else None) for k, t, req, filt, o in rows]
    cities = [r[0] for r in cur.execute('SELECT id FROM geo_city')]
    distr = {}
    for cid, did in cur.execute('SELECT city_id, id FROM geo_district'):
        distr.setdefault(cid, []).append(did)

    # seller pool
    m = a.users or max(50, min(a.count // 5, 50000))
    base = int(cur.execute("SELECT count(*) FROM cel_users WHERE email LIKE 'fake+%@seed.local'").fetchone()[0])
    users = [(str(uuid.uuid4()), f'fake+{base+i}@seed.local', 'user') for i in range(m)]
    cur.executemany('INSERT OR IGNORE INTO cel_users(id,email,role) VALUES(?,?,?)', users)
    seller_ids = [u[0] for u in users]
    print(f'+ {len(users)} sellers')

    # FTS fast path: drop triggers, bulk load, rebuild, recreate
    trig = cur.execute("SELECT sql FROM sqlite_master WHERE type='trigger' AND name LIKE 'listings_fts%'").fetchall()
    for name in ('listings_fts_ai', 'listings_fts_ad', 'listings_fts_au'):
        cur.execute(f'DROP TRIGGER IF EXISTS {name}')

    cols = ('id','region','seller_id','category_id','title','description','price','price_negotiable',
            'currency','locale','city_id','district_id','condition','status','allow_chat','allow_call',
            'allow_whatsapp','photos','attributes','created_at','updated_at','expires_at')
    ins = f"INSERT INTO listings({','.join(cols)}) VALUES({','.join('?'*len(cols))})"
    now = int(time.time())
    BATCH = 20000
    made = 0
    while made < a.count:
        rows = []
        for _ in range(min(BATCH, a.count - made)):
            cid = rnd.choice(cats)
            attrs = {}
            for (k, t, req, filt, opts) in schema[cid]:
                # always fill required + filterable (so facets are dense); ~70% of the rest
                if req or filt or rnd.random() < 0.7:
                    v = gen_attr((k, t, opts), rnd)
                    if v is not None:
                        attrs[k] = v
            plo, phi = PRICE.get(cid, (100, 1_000_000))
            ts = now - rnd.randint(0, 90 * 86400)
            city = rnd.choice(cities) if cities else None
            dist = rnd.choice(distr[city]) if city in distr and rnd.random() < 0.6 else None
            seller = a.owner if (a.owner and rnd.random() < a.owner_share) else rnd.choice(seller_ids)
            photos = json.dumps(rnd.sample(media_pool, min(len(media_pool), rnd.randint(1, 4)))) \
                     if media_pool else '[]'
            rows.append((
                str(uuid.uuid4()), a.region, seller, cid,
                title_for(cid, cat_name.get(cid, 'Объявление'), attrs, rnd),
                rnd.choice(ADJ).capitalize() + '.', rnd.randint(plo, phi), 1 if rnd.random() < 0.3 else 0,
                'KGS', 'ru', city, dist, rnd.choice(('new', 'used', None)), 'active',
                1, 1, 1 if rnd.random() < 0.3 else 0, photos, json.dumps(attrs, ensure_ascii=False),
                iso(ts), iso(ts), iso(ts + 30 * 86400),
            ))
        cur.executemany(ins, rows)
        made += len(rows)
        if made % 100000 == 0 or made == a.count:
            con.commit(); print(f'  {made:,}/{a.count:,} listings')

    # derived facet index (set-based; same logic as the after() hook), then FTS rebuild
    print('building listing_facet…')
    cur.execute("""
        INSERT OR IGNORE INTO listing_facet (listing_id, key, num_value, text_value)
        SELECT l.id, ca.key,
               CASE WHEN ca.type IN ('int','number','bool') THEN json_extract(l.attributes,'$."'||ca.key||'"') END,
               CASE WHEN ca.type NOT IN ('int','number','bool') THEN json_extract(l.attributes,'$."'||ca.key||'"') END
        FROM listings l JOIN category_attribute ca ON ca.category_id=l.category_id AND ca.filterable=1
        WHERE json_extract(l.attributes,'$."'||ca.key||'"') IS NOT NULL""")
    print('rebuilding FTS…')
    cur.execute("INSERT INTO listings_fts(listings_fts) VALUES('rebuild')")
    for sql in trig:
        if sql and sql[0]: cur.execute(sql[0])
    con.commit()

    n = cur.execute('SELECT count(*) FROM listings').fetchone()[0]
    f = cur.execute('SELECT count(*) FROM listing_facet').fetchone()[0]
    con.execute('PRAGMA wal_checkpoint(TRUNCATE)')
    con.close()
    print(f'done: {n:,} listings, {f:,} facet rows in {a.db}')

if __name__ == '__main__':
    sys.exit(main())
