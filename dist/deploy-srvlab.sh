#!/usr/bin/env bash
# deploy-srvlab.sh — push the current cellar working tree to the srvlab box and
# refresh the running service. Idempotent: safe to re-run.
#
#   Engine   : rsync src → rebuild build-cmake → restart cellar.service
#   Bundles  : copy each managed bundle's hooks.lua + policies.json into its
#              live dir under $APPS (code only — never touches data.db or media)
#
# Runtime state that is NOT deployed (kept on the box): data.db*, media/, public/,
# build dirs, .run/, .git, examples/. Data/media are seeded/managed out-of-band.
#
# Usage:  dist/deploy-srvlab.sh [--no-restart] [--engine-only] [--bundles-only]
#                               [--spa] [--spa-only]
#   --spa       also sync built frontend dist → bundle public/ (opt-in; needs a fresh build)
#   --spa-only  ONLY push the SPA (no engine/bundle/restart) — for a frontend-only redeploy
# Requires: ssh access to $SRV + passwordless sudo there (systemctl restart).
set -euo pipefail

SRV="${CELLAR_SRV:-racytech@192.168.50.231}"
REMOTE_SRC="${CELLAR_REMOTE_SRC:-/home/racytech/workspace/cellar}"
APPS="${CELLAR_APPS_DIR:-/home/racytech/cellar-apps}"
PORT="${CELLAR_PORT:-8443}"          # TLS port the service listens on
SERVICE="${CELLAR_SERVICE:-cellar}"

# Managed bundles: "<repo-bundle-dir>=<deployed-host>". The frontend-owned
# clerkhalls bundle (portico-test) is intentionally NOT listed — we don't manage it.
MANAGED_BUNDLES=(
  "apps/classifieds=portico-second.duckdns.org"
)

# Managed SPAs — built frontend artifacts that live OUTSIDE this repo:
# "<dist-dir>=<deployed-host>". Synced into the bundle's public/ (--delete, so
# public/ == dist). Opt-in via --spa / --spa-only because it depends on the
# frontend workspace having a FRESH build; a stale/absent dist is skipped with a warning.
MANAGED_SPAS=(
  "$HOME/workspace/frontend-apps/apps/classifieds/dist=portico-second.duckdns.org"
)

DO_ENGINE=1; DO_BUNDLES=1; DO_RESTART=1; DO_SPA=0
for a in "$@"; do case "$a" in
  --no-restart)   DO_RESTART=0 ;;
  --engine-only)  DO_BUNDLES=0; DO_SPA=0 ;;
  --bundles-only) DO_ENGINE=0; DO_SPA=0 ;;
  --spa)          DO_SPA=1 ;;
  --spa-only)     DO_ENGINE=0; DO_BUNDLES=0; DO_SPA=1; DO_RESTART=0 ;;
  *) echo "unknown flag: $a" >&2; exit 2 ;;
esac; done

HERE="$(cd "$(dirname "$0")/.." && pwd)"   # repo root
cd "$HERE"

say() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }

if [ "$DO_ENGINE" = 1 ]; then
  say "sync source → $SRV:$REMOTE_SRC"
  rsync -az --info=stats1 \
    --exclude='.git/' \
    --exclude='build-cmake/' --exclude='build-asan/' \
    --exclude='**/.run/' \
    --exclude='examples/' \
    --exclude='**/*.db' --exclude='**/*.db-wal' --exclude='**/*.db-shm' \
    --exclude='**/media/' \
    --exclude='**/__pycache__/' \
    ./ "$SRV:$REMOTE_SRC/"

  say "rebuild engine on $SRV"
  ssh "$SRV" "cd '$REMOTE_SRC' && cmake --build build-cmake -j\"\$(nproc)\"" | tail -3
fi

if [ "$DO_BUNDLES" = 1 ]; then
  for map in "${MANAGED_BUNDLES[@]}"; do
    src="${map%%=*}"; host="${map##*=}"
    say "deploy bundle code: $src → $APPS/$host"
    # copy code only; the bundle's data.db/media/public stay as they are
    rsync -az --info=stats1 \
      "$src/hooks.lua" "$src/policies.json" \
      "$SRV:$APPS/$host/"
  done
fi

if [ "$DO_SPA" = 1 ]; then
  for map in "${MANAGED_SPAS[@]}"; do
    dist="${map%%=*}"; host="${map##*=}"
    if [ ! -f "$dist/index.html" ]; then
      echo "  ⚠ skip SPA for $host: no build at $dist (run the frontend build first)" >&2
      continue
    fi
    say "deploy SPA: $dist → $APPS/$host/public/"
    # static files served per-request from disk; no restart needed. --delete keeps
    # public/ an exact mirror of dist (drops stale hashed assets + the provision starter).
    rsync -az --delete --info=stats1 "$dist/" "$SRV:$APPS/$host/public/"
  done
fi

if [ "$DO_RESTART" = 1 ]; then
  say "restart $SERVICE.service"
  ssh "$SRV" "sudo systemctl restart '$SERVICE' && sleep 2 && systemctl is-active '$SERVICE'"
  say "health check"
  # verify each managed host resolves + is healthy through the local TLS listener
  for map in "${MANAGED_BUNDLES[@]}"; do
    host="${map##*=}"
    ssh "$SRV" "curl -sk --max-time 6 -H 'Host: $host' https://127.0.0.1:$PORT/health -w ' <-%{http_code}\n' -o /dev/null" \
      | sed "s#^#  $host #"
  done
fi

say "done"
