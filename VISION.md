# pgforge + portico — Vision & Roadmap

What we want each component to be, where the boundary sits, and what's next. This is
the forward-looking "clear picture" doc; `PLAN.md` is the historical implementation log,
and `SECURITY_AUDIT.md` tracks the (completed) security remediation.

---

## The one principle: mechanism (portico) vs policy (pgforge)

- **portico is the MECHANISM** — the transport. It knows bytes, sockets, framing, TLS,
  the event loop, backpressure. It knows **nothing** about auth, tenants, SQL, or
  policy. It is reusable by *any* HTTP/WS application.
- **pgforge is the POLICY** — the engine. Auth, RLS/tenancy, the policy engine, generic
  CRUD, realtime, the data model. It *uses* portico for transport.

**The decision rule for any new capability:**
> "Could any HTTP/WS app need this, independent of pgforge's data model?" → **portico.**
> "Is it about identity, data, or access policy?" → **pgforge.**

Keep this clean. No pgforge-isms (auth, tenant, SQL) leak into portico; portico stays a
standalone library with its own tests and identity.

---

## portico — the transport library

### What it IS (today)
- HTTP/1.1 + WebSocket (RFC 6455) server as a single-binary C library.
- Edge-triggered epoll, thread-per-core event loop, non-blocking I/O with per-connection
  output backpressure (EPOLLOUT drain).
- Optional TLS (single cert today — see roadmap).
- Hardened post-audit: smuggling-resistant HTTP framing, bounded WS frame/reassembly,
  slowloris reaper, keepalive + idle reaping, fd-reuse-safe connection lifecycle,
  rightmost-XFF client-IP resolution, tokenized header matching.

### What we WANT from it
1. **Static file serving primitive** (no nginx in front — portico serves assets directly).
   - Model: portico exposes a primitive (`portico_res_file(req,res,root,urlpath)`) +
     a **traversal-safe path resolver** (decode → `realpath` → prefix-check); the *app*
     decides routing (so static can be auth-gated). Keeps the mechanism/policy split.
   - v1: small files (≤ ~1 MB) served from an **in-memory hot-asset cache** through the
     existing backpressure buffer — covers an SPA frontend completely, no disk I/O on the
     warm path, sidesteps blocking reads on the event thread.
   - v2: `sendfile(2)` streaming on EPOLLOUT for large files (> ~4 MB, the
     `PORTICO_OUT_MAX` ceiling); thread-pool / io_uring for async cold-disk reads.
   - HTTP surface: MIME by extension, `ETag`/`Last-Modified` + 304, `Range`/206, `HEAD`,
     `index.html`, precompressed `.br`/`.gz`.
2. **Multi-cert TLS + SNI** (the blocker for multi-domain HTTPS / virtual hosting).
   - Today: ONE `SSL_CTX`/cert, created once at startup — so two domains cannot share the
     TLS listener. This is the real gap.
   - Want: hold multiple `SSL_CTX` (one per domain), register an SNI callback
     (`SSL_CTX_set_tlsext_servername_callback` → `SSL_set_SSL_CTX`) to pick the cert from
     the ClientHello before the handshake completes; config = list of `(domain,cert,key)`
     + a default; **hot-reload** the cert map on renewal (no restart). Reload must be
     in-use-safe (the H-7/C-2 "don't free something in flight" discipline).
3. **Hardening follow-up**: a libFuzzer harness for the parsers (WS frame, HTTP request,
   base64) — the continuous complement to the one-time audit.
4. **Stay app-agnostic.** Everything above is generic transport; none of it knows pgforge.

---

## pgforge — the application engine

### What it IS (today)
- Schema-driven, generic PostgreSQL CRUD engine over portico, single binary.
- Auth: Argon2id, opaque revocable sessions, TOTP MFA, OIDC sign-in (verified-token).
- Deny-by-default policy engine; Postgres RLS + app-level scope for multi-tenancy.
- Realtime change events (subscribe → publish, membership-re-checked).
- Richer reads: embedding, aggregates, keyset pagination, boolean filter trees.
- Per-bucket rate limiting: strict auth bucket + a tunable data-API/RPC bucket
  (`PGF_API_RATELIMIT`, keyed by user id else IP). ✅ shipped
- Self-describing API: `GET /openapi.json` (OpenAPI 3.0 generated from the catalog).
  ✅ shipped
- Audited & fail-closed security posture (see `SECURITY_AUDIT.md` — 30/30 resolved).

### What we WANT from it
1. **Virtual-host routing — serve multiple sites on one IP.** (`Host` is already parsed
   by portico; nothing routes on it yet.) Phased by what "a site" means:
   - **(a) static frontends** — `Host → static_root`; smallest, plugs into portico's
     static primitive.
   - **(b) tenants** — `Host → tenant_id`; mostly a shim over the existing tenancy.
   - **(c) distinct apps** — per-vhost schema/routes/policy/cert; the biggest (pgforge is
     one-schema-per-process today → real per-vhost config feature).
   - **Security:** validate `Host` against an allow-list (unknown → reject/default; never
     trust it — same lesson as the XFF fix). Be deliberate about SNI-vs-Host mismatch.
     Per-vhost isolation must preserve RLS/scope guarantees across vhosts.
2. **Static frontend integration** — call portico's `portico_res_file` as the catch-all
   after API routes. Decide SPA fallback (single `index.html`) vs **SSG/per-route HTML**
   (better SEO + link previews; serves `/about → /about/index.html`). Auth-app frontends
   can stay pure-CSR (SEO irrelevant behind login).
3. **Performance** (attack the *measured* bottleneck — round trips + re-planning, not a
   storage swap):
   - **Prepared-statement cache** (per connection, keyed by SQL text) → kills the repeated
     query planning the profiling showed dominates execution. ✅ shipped — on by default;
     `PGF_PREPARED_STATEMENTS=0` opts out. Observable: `pgf_db_prepared_statements_total`.
   - **libpq pipelining** → collapse the pooled-mode `BEGIN/set_config/query/COMMIT`
     4 round trips into one. ✅ shipped — opt-in (`PGF_DB_PIPELINE=1`) pending soak, since
     it restructures the tenant-scoped txn path. Observable: `pgf_db_pipelined_txns_total`.
   - **Unix-domain socket** to Postgres (cheaper than TCP localhost). ✅ shipped — set
     `PGF_DB_HOST=/var/run/postgresql`; TCP keepalives are dropped and the transport is
     logged at startup.
   - *Next:* extend the prepared-statement cache to the auth/MFA paths (run_rows only
     today); measure the pipelining win under load and consider defaulting it on.
4. **Security follow-ups**: CodeQL/Semgrep in CI; share the parser fuzz harness with
   portico; (optional) revisit M-1/M-2 with cross-process cache invalidation if a TTL
   bound ever proves insufficient.

---

## The deployment story (what we want from the *combination*)

- **One binary** (pgforge embeds portico), **one IP**, **no nginx / no reverse proxy** —
  portico serves API + WebSocket + static assets + TLS directly.
- **Multi-site on one IP** via `Host` routing (HTTP) + SNI (HTTPS).
- Runs on a **small VPS (2 vCPU / 8 GB)** or **Oracle Ampere ARM** (code is ARM-clean —
  verified: SIMD is `#ifdef`-guarded with a portable scalar fallback, no x86 lock-in).
- **One pgforge process + Postgres** (separate process). Workload is DB-round-trip bound,
  so a small box serves high concurrency; the first scaling move is putting **Postgres on
  its own node**. Argon2id login is the one genuinely CPU-bound path (rate-limited).

---

## Suggested near-term order

1. **portico static-serving primitive** (path resolver + in-memory cache + `res_file`) —
   unblocks serving a frontend without nginx.
2. **pgforge Host→static-root routing** + Host allow-list — multi static-site, the (a) case.
3. **portico SNI / multi-cert** — unblocks multi-domain HTTPS.
4. **pgforge perf**: prepared statements, then pipelining — the deferred optimization work.
5. **Hardening**: fuzz harness + CodeQL/Semgrep in CI.
6. Later/biggest: per-vhost distinct-app config (case (c)), SSG integration, sendfile
   streaming, async file I/O.
