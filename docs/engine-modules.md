# cellar engine modules & ports

> **Intent:** the new engine primitives cellar needs (first surfaced by the classifieds design —
> see [`classifieds-design.md`](classifieds-design.md) §5 + roadmap), framed as **independent,
> swappable modules behind interfaces**. This is reusable engine work, not classifieds-specific.

## Principle: port-first (the lesson from SQLite)

The SQLite mistake wasn't *using* SQLite — it was letting **storage assumptions leak through the
codebase** (no clean port), so the engine can't swap the store later. Every new primitive here is
built the opposite way:

1. **Define the port (interface) first**, then ship a **default adapter** behind it.
2. The engine depends on the **interface**, never the impl.
3. **No impl assumptions leak into the port** — no SQL in `JobQueue`, no filesystem paths in
   `BlobStore`. That leak is precisely what makes a thing un-swappable.

In C a port is a **struct of function pointers + an opaque `ctx`** (a vtable):

```c
typedef struct {
  void *ctx;
  int   (*put)(void*, const char *key, const void*, size_t);
  int   (*get)(void*, const char *key, buf_t *out);
  char *(*url)(void*, const char *key);
  int   (*del)(void*, const char *key);
} blob_store_t;          // default: local disk; later: S3/R2 — engine never knows which
```

Existing pattern to copy: **`core/mailer.c`** is already a notification-channel-shaped adapter —
push/SMS should sit beside it behind one port. Cautionary tale to *not* repeat: the
`app_db`/`db_sqlite` data layer (the coupling we're avoiding here).

## Independent modules — build + unit-test standalone, behind a port

No cross-dependencies → each can be built and tested **in isolation, before the app exists**.

| Module | Port | Default adapter | First needed |
|---|---|---|---|
| Blob storage | `BlobStore` (put/get/url/del) | local disk → S3/R2 + CDN | P1 (media) |
| Image processing | pure lib (decode/resize/thumbnail/encode) | — (no swap) | P1 |
| Image pHash | pure lib (hash/compare) | — (no swap) | P3 (dup/stolen-photo) |
| Jobs / scheduler | `JobQueue` (enqueue/process/cron/retry) | SQLite table + worker → Redis/NATS | P2 |
| Event ingest | `EventSink` (emit) | SQLite / append-log → stream/warehouse | P2 |
| Captcha | `CaptchaVerifier` (verify token) | Turnstile / hCaptcha | P2 |
| Notifications | `NotifChannel` (send) | email = `mailer.c` (have); **push** = Web-Push/FCM (new); SMS = optional | P2 (push) |
| Passkeys / WebAuthn | self-contained ceremony + `CredentialStore` port | credential storage behind port | P2 (optional) |
| Webhook intake | `verify()` fns (per provider) + `IdempotencyStore` port | SQLite | P4 (payments) |

## Coupled to the engine core — NOT standalone modules

These extend existing subsystems; they can't be side modules.

- **Faceted-query support** ⚠️ — *is* the data/query layer (SQL over the facet-index + FTS5). Lives
  **behind the data/search port**, not beside it. This is the **SQLite-adjacent coupling to design
  deliberately**: build it as part of that port from day one so a future SQLite→Postgres / search-engine
  swap takes it along. (Hand-write the SQL in a hook first if needed; promote to engine if it gets hot.)
- **Untrusted-code sandbox** ⚠️ — hardens the *existing* LuaJIT hook engine (`cel_hooks`/`cel_lua`);
  a modification of core hook execution, not a bolt-on. (Phase 5, far off.)

## Media is *mostly* modular (not a monolithic exception)

Split it — only the last part is coupled:
- **Blob storage** → the cleanest port of all (table above). ✅
- **Image processing** (resize/thumbnail) → a pure standalone lib. ✅
- **Upload + serve HTTP handlers** → the *only* coupled part (multipart, range requests, cache
  headers tie to the request loop) — thin glue over the two modular pieces.

## Build order

`BlobStore` + image lib (P1) → `JobQueue` (P2) → `EventSink` (P2) → `CaptchaVerifier` (+ passkeys) (P2)
→ `NotifChannel`/push (P2) → image pHash (P3) → faceted-query *(in the data port)* (as hot) →
webhook verify + idempotency (P4) → untrusted-code sandbox (P5) → scale adapters (P6, measured need).

The independent modules (P1–P4 above, minus faceted-query) can be built **in parallel, each with its
own tests**, then wired in — the "engine-only first, tested in isolation" track.

## Notes

- The **data/search port** is the one big sticky seam (faceted-query, FTS, the catalog store all ride
  it). It's the hardest to retrofit and the most important to get right — see the swap-path table and
  the sharding north star in `classifieds-design.md` (Scaling section).
- Default adapters stay **embedded/simple** (SQLite, local disk, in-process); scale-out adapters
  (S3, Redis, external search) are built only on a *measured* need — same discipline as the
  portico-tunnel parked multi-core work.
