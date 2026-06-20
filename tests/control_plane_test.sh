#!/usr/bin/env bash
# Control-plane registry (design §11): with CEL_CONTROL_DB set, routing is gated to
# REGISTERED + ACTIVE hosts. provision registers; `cellar apps` lists; `suspend`/
# `resume` take an app offline/online on a RUNNING server (status re-read per
# request). control_plane_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: control_plane_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"
export CEL_CONTROL_DB="$APPS/control.db"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$APPS"' EXIT

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }

CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision a.example admin@a secret123 >/dev/null 2>&1
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision b.example admin@b secret123 >/dev/null 2>&1
# a manually-dropped bundle dir that was NEVER provisioned/registered
mkdir -p "$APPS/ghost.example"

# `cellar apps` lists both registered apps
LIST="$(CEL_LOG_LEVEL=error "$BIN" apps)"
chk "registry lists a.example" "$(echo "$LIST" | grep -c 'a.example .*active')" "1"
chk "registry lists b.example" "$(echo "$LIST" | grep -c 'b.example .*active')" "1"

PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 "$BIN" >/dev/null 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
# served? a registered+active host reaches auth (200/401); a blocked host is 404
served() { curl -s -o /dev/null -w '%{http_code}' -H "Host: $1" -X POST -H 'Content-Type: application/json' \
                -d '{"email":"x@x","password":"secret123"}' "http://127.0.0.1:$PORT/auth/login"; }

chk "registered active app serves (not 404)" "$([ "$(served a.example)" != "404" ] && echo yes || echo no)" "yes"
chk "unregistered dir is gated -> 404"       "$(served ghost.example)" "404"

# suspend on the RUNNING server takes effect immediately (status re-read per request)
CEL_LOG_LEVEL=error "$BIN" suspend a.example >/dev/null 2>&1
chk "suspended app -> 404 (live)"            "$(served a.example)" "404"
chk "other app unaffected (still serves)"    "$([ "$(served b.example)" != "404" ] && echo yes || echo no)" "yes"

# resume restores it
CEL_LOG_LEVEL=error "$BIN" resume a.example >/dev/null 2>&1
chk "resumed app serves again (not 404)"     "$([ "$(served a.example)" != "404" ] && echo yes || echo no)" "yes"

# suspend of an unregistered host reports no-op
if CEL_LOG_LEVEL=error "$BIN" suspend nope.example >/dev/null 2>&1; then
    chk "suspend unknown app -> non-zero" "ok" "nonzero"
else
    chk "suspend unknown app -> non-zero" "nonzero" "nonzero"
fi

[ "$fail" = 0 ] && echo "CONTROL PLANE PASS" || { echo "CONTROL PLANE FAIL"; exit 1; }
