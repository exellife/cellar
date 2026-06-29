#!/usr/bin/env bash
# Safely apply pending app-schema migrations to a cellar bundle.
#
# A schema change (e.g. ALTER TABLE) takes a write lock, so it must not contend
# with a live, writing server. This wrapper enforces the safe order:
#     stop the service  ->  cellar migrate <host>  ->  start the service
# `cellar migrate` itself takes a consistent VACUUM-INTO backup to
# <bundle>/.backups/ before applying anything, so a bad migration is recoverable.
#
# Usage:   cellar-migrate.sh <host> [service-name] [cellar-binary]
#   <host>          the bundle's Host dir under CEL_APPS_DIR
#   service-name    systemd unit (default: cellar)
#   cellar-binary   path/name of the cellar binary (default: cellar on PATH)
# Env:
#   CEL_APPS_DIR    REQUIRED — the multi-app bundles directory
#   CEL_HEALTH_URL  optional — curl'd after restart (expects 200), e.g.
#                   https://127.0.0.1:8443/health
set -euo pipefail

HOST="${1:?usage: cellar-migrate.sh <host> [service] [cellar-binary]}"
SVC="${2:-cellar}"
BIN="${3:-cellar}"
: "${CEL_APPS_DIR:?set CEL_APPS_DIR (the multi-app bundles directory)}"

echo "==> stopping '$SVC' to quiesce writes"
sudo systemctl stop "$SVC"

echo "==> cellar migrate '$HOST'"
if ! CEL_APPS_DIR="$CEL_APPS_DIR" "$BIN" migrate "$HOST"; then
    echo "!! migration FAILED — the failing migration rolled back; restarting '$SVC' on the" >&2
    echo "   unchanged DB. The pre-migrate backup is in <bundle>/.backups/ if you need it." >&2
    sudo systemctl start "$SVC"
    exit 1
fi

echo "==> starting '$SVC'"
sudo systemctl start "$SVC"
sleep 1
if ! systemctl is-active --quiet "$SVC"; then
    echo "!! '$SVC' is not active after start — check: journalctl -u $SVC -n 50" >&2
    exit 1
fi

if [ -n "${CEL_HEALTH_URL:-}" ]; then
    for _ in $(seq 1 30); do
        [ "$(curl -sk -m3 -o /dev/null -w '%{http_code}' "$CEL_HEALTH_URL" 2>/dev/null)" = 200 ] && {
            echo "==> health OK ($CEL_HEALTH_URL)"; break; }
        sleep 0.3
    done
fi

echo "==> done: '$HOST' migrated, '$SVC' back up."
