# cellar — deployments & infrastructure

Operational inventory of where cellar runs and how the pieces connect. **No secrets here** —
credentials live in root-owned systemd drop-ins on each box (`/etc/systemd/system/cellar.service.d/`)
and in Cloudflare/AWS/Google consoles. This file is the map, not the vault.

Deploy tooling: [`deploy-srvlab.sh`](./deploy-srvlab.sh) (rsync source → rebuild → sync bundle code →
restart), [`cellar.service`](./cellar.service) (unit template), [`cellar.env.example`](./cellar.env.example)
(all env vars). cellar routes by **Host** → a bundle dir under `CEL_APPS_DIR`; each bundle is
`schema.sql` (applied into `data.db`) + `hooks.lua` + `policies.json` + `public/` + `media/`.

---

## Servers

### 1. Tunnel relay — Oracle VM `130.61.190.60`
Public ingress for the home box. Runs `portico-tunnel` in **relay** mode; the srvlab agent dials in
over mTLS (control `:7443`). The relay does **SNI passthrough** (routes `:443` by TLS SNI to the
registered agent **without decrypting**) — so TLS terminates at cellar, not here. Public DNS names
(DuckDNS) point their A record at this IP.

### 2. srvlab — home box `racytech@192.168.50.231` (LAN)
The current cellar host. **Residential** (power/network not 24/7 — has gone down mid-work).
- Service: `cellar.service` → `/home/racytech/workspace/cellar/build-cmake/cellar`
- `CEL_PORT=8443` (TLS on), cert `/etc/portico/cert.pem` (LE, SANs: `portico-test.duckdns.org`,
  `portico-second.duckdns.org`), `CEL_APPS_DIR=/home/racytech/cellar-apps`
- **No `CEL_CONTROL_DB`** → host→bundle resolution is **filesystem-based** (a dir under
  `cellar-apps/` = a live Host).
- Config drop-ins (`/etc/systemd/system/cellar.service.d/`): `cors.conf` (`CEL_CORS_ORIGINS=*`),
  `oauth.conf` (Google), `smtp.conf` (SES creds — secret, chmod 600).
- Exposure: `portico-agent.service` (tunnel agent) → relay, registers `portico-test` + `portico-second`.
- **Apps served:**
  - `portico-test.duckdns.org` → **clerkhalls** (frontend-owned demo)
  - `portico-second.duckdns.org` → **classifieds** ("Jarchy") — live SPA + API, seeded (300 listings),
    Google OAuth, SES email. Logins: `admin@classifieds.local`/`classifieds01`,
    `seller@`/`buyer@classifieds.local`/`test1234` (dev creds; also printed by `apps/classifieds/run.sh`).
- srvlab's tree is **not** a git checkout — deploy = rsync from a workstation + rebuild (see deploy-srvlab.sh).

### 3. Oracle Ampere A1 — `130.61.21.191`  (aarch64, Always-Free, PAYG account)  ★ SECONDARY / DEMO
Cloud box, **always-on**. Second direct-serve cellar host (has its own public IP — **no relay**).
- Specs: **4 OCPU / 24 GB**, Ubuntu 24.04 (aarch64), ~45 GB disk. Fits the Always-Free A1 allowance ($0).
- Access: ssh alias **`oracle-a1`** (`~/.ssh/config`) → user `ubuntu`, key `~/.ssh/id_ed25519`.
  Passwordless sudo. The `ubuntu` user's VCN default security list already allows ingress `:443`.
- **cellar RUNNING as a service (2026-07-06):**
  - `cellar.service` → `/usr/local/bin/cellar`, runs as non-root user **`cellar`** (uid 999), binds **:443**
    via `AmbientCapabilities=CAP_NET_BIND_SERVICE`. Minimal unit — **no** CORS/OAuth/SMTP drop-ins
    (clerkhalls is same-origin + admin-provisioned + no email flows). `CEL_APPS_DIR=/var/lib/cellar/apps`
    (filesystem host routing). Inline `Environment=` in the unit (like Hetzner), not an EnvironmentFile.
  - **TLS: real LE `*.svngn.com` cert** (DNS-01 via Cloudflare). `certbot` + `python3-certbot-dns-cloudflare`,
    token at `/root/.secrets/cloudflare.ini` (chmod 600, **reused from Hetzner** — same zone token, copied
    box-to-box). Cert name `svngn.com`, exp **2026-10-04**, `certbot.timer` auto-renews. Deploy hook
    `/etc/letsencrypt/renewal-hooks/deploy/cellar.sh` copies `fullchain`/`privkey` →
    `/etc/cellar/tls/{cert,key}.pem` (chown cellar) + `systemctl restart cellar`.
  - Local firewall: iptables ACCEPT for tcp/443 inserted before the image's trailing REJECT, persisted
    via `netfilter-persistent`.
- **Apps (host-routed bundles under `/var/lib/cellar/apps/`):**
  - `clerkhalls.svngn.com` — **ClerkHalls** (venue/hall booking SPA + API, same-origin). **LIVE (2026-07-06).**
    Trilingual EN/KY/RU. Bundle source of truth: `frontend-apps/apps/clerkhalls/bundle/` (its `DEPLOY.md` is
    the contract). Binary requirement met by rebuild from cellar HEAD (device tokens `1cc0e20` +
    `cellar.set_password` `13631b5` → PIN fast-sign-in + staff password reset). Provisioned fresh (no seed
    data): admin `admin@clerkhalls.svngn.com`, read-only `owner@clerkhalls.svngn.com` (both pw in the vault,
    `app_logins`). **Self-register is OFF** — managers/clerks created from the in-app Staff page. No jobs/
    email/OAuth. **Deploy backend:** rsync `bundle/` → box; `cellar provision <host> <admin> <pw>` →
    `sqlite3 data.db < schema.sql` → `cp -r public hooks.lua policies.json` → chown cellar → restart; seed
    owner via `/auth/users`. **Redeploy client:** `pnpm --filter clerkhalls build` → `cp -r dist/* bundle/public/`
    → rsync → atomic swap into `public/`.
- **DNS:** Cloudflare **specific** A record `clerkhalls.svngn.com` → `130.61.21.191` (grey/DNS-only, TTL 300)
  overrides the `*.svngn.com` wildcard (→ Hetzner). Adding it did **not** disturb svngn/jarchy/tandem.
- Source at `~/workspace/cellar` (+ sibling `~/workspace/portico`), synced via rsync from the workstation;
  binary built natively on aarch64 (must build on-box — can't copy the x86-64 Hetzner binary). 80/80 ctest.

### 4. Hetzner — `89.167.89.235`  ★ PRIMARY / PROD  (x86-64, Helsinki `hel1`, paid ~€/mo)
Cloud box, **always-on**. Hostname `ubuntu-8gb-hel1-2`. **Designated production host** (jarchy +
personal page + web pages). Oracle A1 is dev/demo.
- Specs: **4 vCPU / 8 GB**, Ubuntu 26.04 LTS (x86-64), 75 GB disk. IPv4 + IPv6 (`2a01:4f9:c014:37cd::1`).
- CPU: AMD EPYC-Rome (KVM, shared vCPU line), AES-NI/SHA-NI/AVX2. No swap.
- Access: ssh alias **`hetzner`** → user **`root`** (Hetzner default; no `ubuntu` user), key `~/.ssh/id_ed25519`.
- Build validated: 80/80 ctest on the newest toolchain (gcc 15.2, cmake 4.2). One fix it surfaced:
  `migrate_test.c` needed `#include <stdlib.h>` for `mkdtemp` (gcc 14+/C23) — `2cdfc71`.
- **cellar RUNNING as a service (2026-07-02):**
  - `cellar.service` → `/usr/local/bin/cellar`, runs as non-root user **`cellar`**, binds **:443**
    directly via `AmbientCapabilities=CAP_NET_BIND_SERVICE` (no relay — public IP).
  - `CEL_APPS_DIR=/var/lib/cellar/apps` (no control db → filesystem host routing).
  - Drop-ins (`cellar.service.d/`): `cors.conf` (`*`), `oauth.conf` (Google), `smtp.conf` (SES — chmod 600).
  - **SES on port 2465** (not 465): Hetzner blocks outbound SMTP (25/465/587) by anti-spam policy even
    with no firewall outbound rules. AWS SES's alternate port **2465** (implicit TLS) bypasses it —
    verified with a real send. (srvlab uses 465; the 2465 workaround is Hetzner-specific.)
  - Binary installed to `/usr/local/bin/cellar` (copied from the build); deploy = rebuild + re-copy + restart.
- **TLS: real Let's Encrypt `*.svngn.com` + `svngn.com` cert** (DNS-01 via Cloudflare). `certbot` +
  `python3-certbot-dns-cloudflare`, token in `/root/.secrets/cloudflare.ini` (chmod 600).
  **Auto-renews** (`certbot.timer`); deploy hook `/etc/letsencrypt/renewal-hooks/deploy/cellar.sh`
  copies `fullchain`/`privkey` → `/etc/cellar/tls/{cert,key}.pem` (chown cellar) + `systemctl restart cellar`
  (cellar has no SIGHUP handler yet → restart, ~2s blip every ~60d; SIGHUP-reload is a backlog item).
- **DNS:** Cloudflare A records `svngn.com` + `*.svngn.com` → `89.167.89.235` (grey / DNS-only, TTL 300).
- **LIVE (2026-07-02):** `https://svngn.com/` → 200 with a valid cert, serving a placeholder welcome page
  (bundle `svngn.com`, admin `admin@svngn.com`). Apex + wildcard both serve.
- **Apps (host-routed bundles under `/var/lib/cellar/apps/`):**
  - `svngn.com` — placeholder welcome page (admin `admin@svngn.com`).
  - `tandem.svngn.com` — **pan_web** (the `tandem` bundle: `submit_inquiry` + admin console:
    inquiries / site settings / user mgmt) **+ the Nuxt static site, same-origin**. **FULLY LIVE
    (2026-07-03)** — `/` serves the site, `/admin` the console, `/rpc/*` the API; SPA deep-links
    fall back OK. Admin `admin@tandem.svngn.com` (pw in the vault, `app_logins`). No DNS/cert work
    needed — `*.svngn.com` A-record + wildcard cert cover it; filesystem routing lazy-opens the app
    (no service restart). **Redeploy backend:** `pan_web/backend/tandem/deploy.sh` with
    `CEL_APPS_DIR=/var/lib/cellar/apps CELLAR_BIN=/usr/local/bin/cellar TANDEM_HOST=tandem.svngn.com`,
    run as the `cellar` user (set `CEL_PORT` to a free port so the write-lock guard passes — prod is on :443).
    **Redeploy site:** `nuxt generate` → rsync `.output/public/` to the box → atomic swap into the
    bundle's `public/` (chown `cellar:cellar`); prior `public` kept as `public.old` for rollback.
  - `jarchy.svngn.com` — **classifieds** (the `apps/classifieds` bundle: catalog + search/facets +
    listings + media + chat + favorites/saved-searches/notifications) **+ the Vite SPA client**
    (`frontend-apps/apps/classifieds`, same-origin). **LIVE (2026-07-03).** Provisioned with the real
    KG seed (`seed.sql`: 14 categories + geo tree) — **not** `seed_fake.py`, and **not** the dev
    seller/buyer test users. Admin `admin@jarchy.svngn.com` (pw in the vault, `app_logins`). Recurring
    jobs enqueued: `expire_listings` (86400s) + `match_saved_searches` (900s) — driven by the process
    worker (`CEL_JOBS_INTERVAL=30`, on by default). Media/blobs at `<app>/media` (per-app disk). Inherits
    process-wide SES + Google OAuth. No DNS/cert work (wildcard covers it); lazy app-open, no restart.
    **Deploy backend:** `cellar provision jarchy.svngn.com <admin> <pw>` → `sqlite3 data.db < schema.sql`
    → `< seed.sql` → copy `hooks.lua`/`policies.json` → `mkdir media`, run as the `cellar` user.
    **Deploy client:** `pnpm build` (client id defaults to the real one in `src/lib/config.ts`, `.env.local`
    is `VITE_MOCK=false`) → rsync `dist/` → atomic swap into `public/`.
    - **Trust & safety:** report → moderation queue → takedown is **LIVE (2026-07-03)** — `listing_report`
      table + `listings.pre_moderation_status` + the `report_listing`/`list_reports`/`takedown`/`reinstate`/
      `dismiss` rpcs (hardened via adversarial review, commit `9ca89cb`). Auto-hide (`CLS_AUTO_HIDE_REPORTS`)
      is **off** by default → manual moderation. **Client UI still to build** (report button + admin queue —
      contract in `apps/classifieds/FRONTEND.md`).
    - **Full stack LIVE (2026-07-05):** the accumulated classifieds work shipped to prod in one coordinated
      deploy — new **binary** (6-digit email verification + per-recipient mail throttle + atomic auth-schema
      migration; auth schema auto-migrated **v4→v6** on first request), full **bundle** (`is_free` +
      31-subcat taxonomy + the verify **gate** + moderation), the new **frontend**, and `CEL_REGISTER_AUTOLOGIN=1`
      (drop-in). Verify gate is **on by default** (`CLS_REQUIRE_VERIFIED`) — a real signup now emails a 6-digit
      code via SES; posting/contact require verification (admins exempt). jarchy had **0 listings**, so the
      catalog delta (`is_free` ALTER + taxonomy INSERTs + `deal` 3-way) was applied in place after a backup.
      Deploy = rebuild binary on the box + `cp /usr/local/bin/cellar` + swap bundle + apply delta + publish
      dist + restart (blips svngn/tandem ~3s). *(This `is_free`/taxonomy delta was hand-applied — the LAST
      hand-applied one; the bundle adopted a `migrations/` dir in the recs-slice deploy below.)*
    - **Behavioral tracking + recs rollup + migrations LIVE (2026-07-08, commit `fb5ab84`):** the
      "personalization-ready" slice — per-user event capture (`search` now stamps the actor + fires on a
      category browse, not just a text query), a **`track` rpc** (client impression/result_click/card_view/
      dwell/search_view ingestion; **server-stamps the actor**, type allowlist, batch(20)/props(512B)/rate
      (300-per-min) caps, anon-allowed, fire-and-forget), and a daily **`rollup_interest`** job folding the
      EventSink stream into per-user category affinity (`user_interest`, cursor in `rollup_state`).
      Adversarially reviewed (18 agents → 3 findings fixed: O(n²) scan-DoS, category-injection, pcall).
      **classifieds now has a `migrations/` dir** — `0001_init` (idempotent baseline that adopts live DBs) +
      `0002_recs_tracking` (the 3 new tables) — and this shipped via **`cellar migrate`, the first migrate-based
      prod deploy** (the hand-applied-SQL era is over). **Deploy recipe:** sync bundle (`hooks.lua`/`policies.json`/
      `schema.sql`/`migrations/`) → `systemctl stop cellar` → `CEL_APPS_DIR=… cellar migrate <host>` **run as the
      `cellar` user** (VACUUM-INTO backup to `<bundle>/.backups/`, tracked in `_schema_migrations` w/ checksum +
      drift detection, idempotent) → `systemctl start cellar` → seed `rollup_interest` (`repeat_every` 86400).
      `dist/cellar-migrate.sh` wraps stop→migrate→start, **but runs migrate as the invoking user** — invoke it as
      `cellar` (or migrate manually as `cellar`) so `data.db`/WAL/backup ownership stays correct. **No feed
      consumer yet** (reads `user_interest` to rank) — deferred until real traffic; the profile accrues meanwhile.
      **`track` client contract (endpoint / 5 event types / `props` as a JSON string / batch ≤20) → FEEDBACK.**
    - **Launch follow-up:** add `jarchy.svngn.com` as an **Authorized JavaScript origin** on the Google
      OAuth client, else the Google button fails (email/password unaffected).
- **Footgun noted:** `cellar --version` is not a recognized flag → cellar ignores it and **starts a server
  on the default port 8080**. Don't probe the binary that way (leaves a stray root server); use it only to run.
- **Deferred:** jarchy (classifieds) domain placement + migration from srvlab; real personal page.

---

## Build on a fresh box (reference)

cellar is architecture-clean (builds on x86-64 **and** aarch64; no source changes). LuaJIT is vendored
(`third_party/luajit`, built from source); **`portico` is a SIBLING repo** (`../portico`) that must be
present alongside cellar.

```bash
# Ubuntu 24.04 (x86-64 or ARM64)
sudo apt-get install -y build-essential cmake pkg-config \
  libssl-dev libcurl4-openssl-dev libsodium-dev uuid-dev sqlite3 git rsync \
  python3-websockets            # last one: only for the ctest 'smoke' WS test
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTICO_TLS=ON
cmake --build build-cmake -j"$(nproc)"
ctest --test-dir build-cmake    # expect 80/80
```

---

## External services (shared, reusable across all servers)

- **Domain:** `svngn.com` — DNS on **Cloudflare**. (Public apps currently still on the DuckDNS names
  via the relay; migration to `svngn.com` + wildcard cert is a planned track.)
- **Email — Amazon SES** (`eu-central-1`): sending domain `svngn.com` **verified** (DKIM + DMARC pass);
  send as `no-reply@svngn.com`. SMTP creds are in srvlab's `smtp.conf` drop-in. **Sandbox** as of
  2026-07-02 (production-access request pending). Cost ~$0.10/1k. Reusable from any server.
- **Google OAuth:** one Web client `407512097851-…apps.googleusercontent.com`. Configured **process-wide**
  (`CEL_OAUTH_*`), so all apps on one cellar process share it. Verify: `POST /auth/oauth {provider,id_token}`.
- **Config scope caveat:** OAuth + SMTP are **per-process**, not per-bundle — all apps on one cellar share
  one client ID + one `From`. Per-app config is a backlog engine enhancement (only matters when distinct
  brands share a process; separate servers already get separate config).

---

## TLS & multi-domain / multi-server routing

cellar routes by the **Host** header → a bundle dir under `CEL_APPS_DIR`. The host can be **any**
FQDN — a subdomain (`jarchy.svngn.com`) *or* an entirely separate domain (`jarchy.com`). Different
domains are just different bundle dirs; no special handling.

**Splitting subdomains across servers (one domain, many boxes).** A wildcard DNS record points to one
IP, but a **specific record overrides the wildcard**. So in Cloudflare for `svngn.com`:
- `*.svngn.com` + apex → **prod** (Hetzner) — catch-all
- `dev.svngn.com`, `demo.svngn.com` → **Oracle A1** (specific records win over the wildcard)

Both boxes have public IPs, so each serves directly — **no relay needed** (the SNI tunnel/relay exists
only for the residential srvlab box).

**Certs (today).** cellar loads a SINGLE cert via `CEL_TLS_CERT`/`CEL_TLS_KEY`. Two supported patterns:
- **Per-box wildcard via DNS-01 (recommended for multi-server):** each box runs its own
  `certbot`/`acme.sh`/`lego` with the **Cloudflare API** (scoped DNS-edit token for the zone) to obtain
  its own `*.svngn.com` cert. Each **auto-renews itself**; no shared private key, no copying. Wire the
  renewal `--deploy-hook` to reload cellar — **portico hot-reloads TLS on `SIGHUP`**, so renewals are
  zero-downtime. LE issues the same wildcard to multiple certs fine.
- **Single multi-SAN cert:** one cert listing all domains/subdomains (e.g. `svngn.com, *.svngn.com,
  jarchy.com, *.jarchy.com`), copied to each box. Simpler but shares a private key + manual re-copy on
  renewal. (This is how srvlab's cert spans two hostnames.)

**Binding :443.** A direct-public box runs cellar on `CEL_PORT=443`; the systemd unit grants
`AmbientCapabilities=CAP_NET_BIND_SERVICE` so it can bind 443 as a non-root `cellar` user.

**Backlog — auto-TLS:** portico already has **SNI multi-cert + ACME** built in; exposing them through
cellar would auto-issue/renew per-domain certs (making "add a domain" = DNS + a bundle, no certbot).
Not yet wired. See the ACME/SNI backlog note.

---

## Maintenance at a glance

- **Domain renewal** — annual (~$10/yr, Cloudflare auto-renew) — the one thing that breaks everything if it lapses.
- **AWS card valid** — SES suspends on failed payment; volume cost is pennies. Set a Budget alert.
- **Bounce/complaint rates** — near-zero for transactional; SES auto-suppression handles it. Glance occasionally.
- **TLS certs** — if hosting on `svngn.com` with a wildcard cert, automate 90-day LE renewal (certbot/acme timer).
- Otherwise: SES service, DNS records, SMTP creds, OAuth client, cellar config = set-and-forget.
