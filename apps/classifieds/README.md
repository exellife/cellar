# classifieds (cellar app bundle)

A Kyrgyzstan-focused online classifieds platform — **discovery + connection**
(browse / search / contact), not transactions. Built as a cellar **app bundle**:
generic engine primitives composed by this bundle's schema + hooks + policies.
cellar itself never learns the word "listing".

See the design + plan in the repo root:
[`docs/classifieds-design.md`](../../docs/classifieds-design.md) ·
[`docs/build-tasks.md`](../../docs/build-tasks.md) ·
[`docs/engine-modules.md`](../../docs/engine-modules.md).

## Bundle contents

| File | Role |
|---|---|
| `schema.sql` | catalog tables — category / category_attribute (taxonomy as data), listings (+ JSON `attributes`), listing_facet (derived index), geo tree |
| `hooks.lua` | `before` = write-time attribute **validation**; `after` = **facet-sync** (rebuild `listing_facet` from stored JSON); `rpc rebuild_facets` |
| `policies.json` | anon+user browse, user posts, admin curates the taxonomy/geo |
| `seed.sql` | KG geo tree (oblast→city→district) + a starter category taxonomy with attributes |
| `test_phase0.py` | e2e: boots cellar against the bundle, exercises validation + facet-sync (ctest `classifieds_phase0`) |
| `run.sh` | provision + seed + boot for manual use |

## Run it

```sh
cmake --build build-cmake --target cellar      # from the repo root
apps/classifieds/run.sh                         # → http://localhost:8080/
```

## Data model in one breath

The **category_attribute** table is the single source of truth: it drives the
post form, the filters, and the server-side validation/facet hooks — so adding a
category or a field is **data, not code** (no migration, no deploy). Listings
keep common fields as first-class indexed columns and category-specific values in
an `attributes` JSON column; the filterable subset is mirrored into
`listing_facet` for fast faceted filtering. (Full rationale: design doc §6.)

## Status

**Phase 0 complete** (foundations): schema, validation + facet-sync hooks,
policies, KG seed, e2e test. Next (Phase 1): media upload/serve, listing CRUD
API surface, FTS5 search, faceted filtering, browse, contact. See
[`docs/build-tasks.md`](../../docs/build-tasks.md).
