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

### 3. Oracle Ampere A1 — `130.61.21.191`  (aarch64, Always-Free, PAYG account)
Cloud box, **always-on** (candidate to replace residential srvlab for reliability).
- Specs: **4 OCPU / 24 GB**, Ubuntu 24.04 (aarch64), ~45 GB disk. Fits the Always-Free A1 allowance ($0).
- Access: ssh alias **`oracle-a1`** (`~/.ssh/config`) → user `ubuntu`, key `~/.ssh/id_ed25519`.
- **Status (2026-07-02): build + test validated only** — cellar compiles cleanly and passes 80/80
  ctest natively on ARM64. **Not yet running as a service / not serving traffic.**
- Source at `~/workspace/cellar` (+ sibling `~/workspace/portico`), synced via rsync from the workstation.

### 4. Hetzner — `89.167.89.235`  (x86-64, Helsinki `hel1`, paid ~€/mo)
Cloud box, **always-on**. Hostname `ubuntu-8gb-hel1-2`.
- Specs: **4 vCPU / 8 GB**, Ubuntu 26.04 LTS (x86-64), 75 GB disk. IPv4 + IPv6 (`2a01:4f9:c014:37cd::1`).
- Access: ssh alias **`hetzner`** → user **`root`** (Hetzner default; no `ubuntu` user), key `~/.ssh/id_ed25519`.
- **Status (2026-07-02): reachable, bare** — cellar not yet built or deployed here. Role/topology TBD
  (candidate primary host, or second box in the multi-server split).

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

## Maintenance at a glance

- **Domain renewal** — annual (~$10/yr, Cloudflare auto-renew) — the one thing that breaks everything if it lapses.
- **AWS card valid** — SES suspends on failed payment; volume cost is pennies. Set a Budget alert.
- **Bounce/complaint rates** — near-zero for transactional; SES auto-suppression handles it. Glance occasionally.
- **TLS certs** — if hosting on `svngn.com` with a wildcard cert, automate 90-day LE renewal (certbot/acme timer).
- Otherwise: SES service, DNS records, SMTP creds, OAuth client, cellar config = set-and-forget.
