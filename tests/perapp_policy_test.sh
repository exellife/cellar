#!/usr/bin/env bash
# Per-app authorization policy: two apps that differ ONLY in their bundle
# policies.json get different authz for the SAME request — proving policy is bound
# per request (cel_apps_enter) from each app's policies.json, not a global file.
# perapp_policy_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: perapp_policy_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$APPS"' EXIT

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }

CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision a.example admin@a secret123 >/dev/null 2>&1
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision b.example admin@b secret123 >/dev/null 2>&1

# a.example: a policy that opens a 'member' role to self-registration.
# b.example: NO policies.json -> built-in defaults (no self-registerable role).
cat > "$APPS/a.example/policies.json" <<'JSON'
{ "_default": "allow",
  "_roles": { "member": { "self_register": true, "allow": ["list", "get", "create"] } } }
JSON

PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 "$BIN" >/dev/null 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
U="http://127.0.0.1:$PORT"

reg() {  # reg <host> <email>  -> HTTP status of a member self-registration
    curl -s -o /dev/null -w '%{http_code}' -H "Host: $1" -X POST -H 'Content-Type: application/json' \
         -d "{\"email\":\"$2\",\"password\":\"secret123\",\"role\":\"member\"}" "$U/auth/register"
}

A="$(reg a.example m@a)"   # a.example's policy enables member self-registration
B="$(reg b.example m@b)"   # b.example has no such policy -> denied

chk "a.example: member self-register NOT denied" "$([ "$A" != "403" ] && echo ok || echo denied)" "ok"
chk "a.example: 2xx"                              "$([ "$A" -ge 200 ] && [ "$A" -lt 300 ] && echo yes || echo no)" "yes"
chk "b.example: member self-register DENIED 403"  "$B" "403"

# Same binary, same request, same instant — the only difference is each app's
# policies.json. That is per-app policy.
[ "$fail" = 0 ] && echo "PER-APP POLICY PASS" || { echo "PER-APP POLICY FAIL (A=$A B=$B)"; exit 1; }
