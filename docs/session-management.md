# Session management — per-app strategies (design)

> **Status:** design of record (2026-06-30). Implements per-app, configurable session
> management behind an extension seam. Phase 1 (this doc → code): the `SessionStrategy`
> seam + two built-in adapters (`fixed`, `sliding`), selected per app via `policies.json`.
> Code-pluggable strategies (JWT/stateless, external store) are **documented but not built**
> — see [§5](#5-future-code-pluggability-not-built). Companion: [`policy-guide.md`](policy-guide.md),
> [`frontend-guide.md`](frontend-guide.md), the device-token item in [`../FEEDBACK.md`](../FEEDBACK.md).

## 1. Why

Today a session is a fixed **absolute** 24h token: `cel_auth_issue_session` stamps
`expires_at = now + SESSION_TTL_SECONDS` (a `#define (24*3600)` in `src/engine/api.c`),
and `cel_auth_verify` only *checks* `expires_at > now` — it never renews. Two consequences:

- an **active** user is logged out at exactly hour 24, mid-task;
- an **idle/stolen** token stays valid for the full 24h regardless of use.

Different apps want different policies (a banking-style app: short idle timeout + hard cap;
a trusted back-office like ClerkHalls: long, comfortable sessions). cellar is multi-app with
per-app config already (`policies.json` is loaded per app-slot), so **session policy belongs
per app**, and we want to be able to **add new strategies later** without reworking callers.

## 2. The seam

One internal interface; today's logic becomes the default adapter behind it. Callers
(`api.c` login/register/oauth/mfa + the per-request verify path) go through the seam, never a
`#define`.

```c
/* src/core/session.h (new) — the strategy interface. Adapters are C, never Lua:
 * the verify path is the hot, security-critical path. */
typedef struct {
    /* mint a session for an authenticated user; writes the opaque token */
    int  (*issue)(const cel_session_policy_t *pol, const char *user_id,
                  char *out_token, size_t token_size);
    /* resolve a token → user, applying renewal/expiry per the policy. May write
     * (sliding renew). Returns CEL_AUTH_OK / CEL_AUTH_INVALID. */
    int  (*verify)(const cel_session_policy_t *pol, const char *token, cel_user_t *out);
    /* revoke one token (logout). */
    int  (*revoke)(const char *token);
    /* (Phase 2 — refresh/device) exchange a long-lived credential for a session. */
    /* int (*refresh)(const cel_session_policy_t*, const char *cred, char *out, size_t); */
} cel_session_strategy_t;
```

`cel_session_policy_t` is the resolved per-app config (§3). The auth layer keeps
`cel_auth_issue_session` / `cel_auth_verify` as thin wrappers that resolve the active
policy, pick the strategy, and delegate — so the storage (`cel_sessions`), token hashing,
and the session cache stay shared and the adapters only differ in *renewal* and (later)
*issuance shape*.

**Selection** is per request via the already-thread-bound active policy (`policy.c active()`),
so login-time and verify-time both see the right app's `_session` config.

## 3. Per-app config (`policies.json`)

A reserved top-level `_session` block (mirrors how `_roles` / `_default` already live there;
`_roles.self_register` set the precedent for non-authz auth behavior in this file):

```jsonc
"_session": {
  "strategy": "fixed",            // "fixed" (default) | "sliding"
  "ttl_seconds": 86400,           // fixed: absolute lifetime. sliding: the IDLE window.
  "absolute_max_seconds": 0       // sliding only: hard cap measured from created_at; 0 = none
}
```

- **Omitted entirely → today's exact behavior**: `fixed`, 86400s. Existing apps are unchanged.
- **`fixed`**: `expires_at = created_at + ttl_seconds`; verify checks `expires_at > now`. (= today,
  but now per-app configurable instead of a `#define`.)
- **`sliding`**: `ttl_seconds` is the idle window. See §4.
- **Unknown `strategy`** → logged as an error and falls back to the safe default (`fixed`); the
  app still serves rather than failing to open (fail-*safe*: a config typo can't lock everyone out).
- **Defaults when `strategy:"sliding"` but a field is omitted:** `ttl_seconds` → 3600 (1h idle),
  `absolute_max_seconds` → 0 (no cap). *(open to changing these.)*
- **Caveat:** `policies.json` loads **once per app-slot at startup — no hot-reload**. A session-policy
  change needs a cellar restart (same as any policy edit).

Plumbing: a `cel_policy_session(cel_session_policy_t *out)` getter in `policy.c` reads `active()`
and fills the struct (or the built-in default when absent), exactly like `cel_policy_realtime_enabled`.

## 4. Sliding renewal — the mechanics (and the one tricky corner)

`cel_sessions` already has both columns we need — **no schema change**:
- `created_at` = issue time → the absolute ceiling is `created_at + absolute_max_seconds`.
- `expires_at` = the current sliding deadline → renewed by bumping it.

**Verify (sliding):** the session is valid iff
`expires_at > now  AND  (absolute_max == 0  OR  created_at + absolute_max > now)`.

**Lazy renewal (avoid a write per request):** only renew once **half or less** of the idle
window remains — `if remaining*2 <= ttl` (integer math on whole seconds; `<=` so a session
polled exactly every `ttl/2` renews on the tick rather than expiring) — then
`UPDATE cel_sessions SET expires_at = MIN(now + ttl, created_at + absolute_max_or_∞)`.
So a steady stream of requests writes at most ~once per `ttl/2`, not every request.

**The tricky corner — the session cache.** `CEL_SESSION_CACHE_TTL` (opt-in) caches the resolved
user for up to TTL seconds and *skips the DB*, which is exactly where renewal happens. The rule:
- the cache holds the **resolved identity**, never authority over expiry;
- renewal happens only on a **DB verify** (cache miss). With the cache on, a session that crossed
  its idle deadline can still serve from cache until the cache entry lapses — **bounded staleness
  ≤ cache TTL**, the same window already accepted for logout/revocation (a logout evicts locally
  but other instances re-validate within the cache TTL);
- therefore for sliding apps that want tight idle enforcement, keep `CEL_SESSION_CACHE_TTL` small
  (≤ the idle window minus a margin) or off. This trade-off is documented, not hidden.

**Security invariants (all strategies):** token is hashed at rest (unchanged); renewal **never**
extends past the absolute cap; `revoke`/logout still deletes the row + evicts the cache;
an unknown strategy fails *safe* to `fixed` (logged); strategies are C, not hooks.

## 5. Future code-pluggability (NOT built)

The seam in §2 is the stable extension point. Everything above is **config-driven over one shared
storage** (`cel_sessions`) — which covers `fixed`, `sliding`, and the planned **refresh/device
token** (the [`FEEDBACK.md`](../FEEDBACK.md) item; it's a new adapter `+` a `refresh()` method `+`
a long-lived revocable credential table, still SQLite-backed). The following are **structurally
different** — they *replace* the storage assumption rather than tune it — and would each be a new
adapter registered behind `cel_session_strategy_t`, selected by `_session.strategy`. We are **not
building these now**; this section records how they slot in so the seam is designed to admit them.

### 5a. JWT / stateless
- **Shape:** `issue()` returns a **signed** token (HMAC or asymmetric) carrying `{sub, role, iat, exp}`;
  `verify()` validates signature + `exp` with **no `cel_sessions` row** (no DB read on the hot path —
  the scaling win).
- **The catch — revocation.** Statelessness breaks server-side revoke (logout, "kill this device",
  password-reset session purge). Needs a **denylist** (revoked `jti`s until their `exp`) or short
  access-token TTL + refresh. So a JWT adapter is realistically **JWT access + refresh** (ties into 5b's
  credential store), not pure stateless.
- **Config it would add:** signing key/alg (a key-management concern — rotation, per-app vs process key),
  access TTL, denylist store.
- **Why deferred:** only pays off at multi-instance scale or to cut the per-request auth DB hit; adds
  key management + the revocation footgun. Keep the seam.

### 5b. External / multi-instance session store (Redis / another box)
- **Shape:** `issue`/`verify`/`revoke` go to an **external store** instead of `cel_sessions`. This is
  the real unlock for **horizontal scale** — today sessions live in the per-app SQLite (single box,
  single-writer), so two cellar instances can't share sessions.
- **How it slots in:** a small **`SessionStore` port** (get/put/del/touch by token-hash) with a default
  **SQLite adapter** (= today) and a **Redis/remote adapter**; the `sliding`/`fixed` *logic* is unchanged,
  only the storage backend swaps. (Same port+adapter shape as `BlobStore`.) This is the cleanest of the
  three because the strategy logic is reused — only persistence moves.
- **Config it would add:** store URL/credentials; pairs with the broader multi-instance story (shared
  control-plane, sticky vs shared routing) in [`engine-modules.md`](engine-modules.md).
- **Why deferred:** single-box is the current target; revisit with the Phase-6 scale-out work.

### 5c. Proof-of-possession / key-bound tokens (mTLS, DPoP-style)
- **Shape:** the token is **bound to a client key**; `verify()` additionally checks proof-of-possession,
  so a stolen bearer token is useless without the key. Highest assurance.
- **How it slots in:** an adapter that layers a PoP check over any store; needs a client-key enrolment
  step (overlaps the device-token enrolment in the FEEDBACK item).
- **Why deferred:** high implementation + client-integration cost; only for the most security-sensitive
  deployments. Documented as a known extension, not on the roadmap.

**Design rule that keeps all of the above cheap to add:** callers depend on the `cel_session_strategy_t`
interface and the `_session` config dispatch — **never** on `cel_sessions` directly. Storage is an
implementation detail of an adapter. As long as that holds, 5a–5c are additive.

## 6. Plan

1. `cel_session_policy_t` + `cel_policy_session()` getter (parse `_session`, default = fixed/86400).
2. `src/core/session.{h,c}` — the `cel_session_strategy_t` seam + `fixed` and `sliding` adapters
   (built on the existing `cel_sessions` storage + token hashing).
3. Route `cel_auth_issue_session` / `cel_auth_verify` through the seam; drop the `api.c` `#define`
   to a built-in default constant used only when no `_session` config applies.
4. Tests: a per-app `_session` e2e (fixed unchanged; sliding renews an active session, drops an idle
   one, honors the absolute cap; unknown strategy → safe default) + the config-resolution unit test.
   Target: full suite green + ASan-clean.
5. Docs: `policy-guide.md` gains the `_session` block; this file is the design ref. The device-token
   FEEDBACK item becomes the next adapter (`refresh()`), not a separate subsystem.
