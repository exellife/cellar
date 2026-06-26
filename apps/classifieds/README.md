# classifieds (cellar app bundle)

A Kyrgyzstan-focused online classifieds platform — **discovery + connection**
(browse / search / contact), not transactions. Built as a cellar **app bundle**:
generic engine primitives composed by this bundle's schema + hooks + policies.
cellar itself never learns the word "listing".

Design + plan (this dir): [`classifieds-design.md`](classifieds-design.md) ·
[`build-tasks.md`](build-tasks.md). Generic engine ports:
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
| [`FRONTEND.md`](FRONTEND.md) | **front-end integration guide** — API contract, auth, security, and classifieds-specific gotchas for the client dev/agent (web = React, mobile = React Native later) |

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

## Auth

Password + session login works out of the box (`self_register` = `user`).
OAuth/OIDC (e.g. Google) and email magic-link are **config-only** engine
features — a federated/new user lands as role `user`. OAuth needs only the
`CEL_OAUTH_GOOGLE_*` env (no email send); email magic-link is **deferred** until
there's a domain + a transactional provider + SPF/DKIM/DMARC (never direct-send
from the origin — it gets rejected). See `run.sh` and `dist/cellar.env.example`.

Authorization is enforced by `policies.json` + hooks: anon may browse; a logged-in
`user` may post and edits/deletes only **their own** listings (`owner_column` on
`seller_id`); contact/chat/favorites are login-gated; the catalog is admin-curated.

## Status

**Phase 1 (MVP loop) backend complete**: media upload/serve, listing CRUD +
photos, metadata-driven post form, FTS5 search, faceted filtering, browse +
detail, contact (phone-reveal + realtime chat), favorites, auth gates. The web/
mobile client (A1.UI) is parked on the client-stack decision. See
[`build-tasks.md`](build-tasks.md).
