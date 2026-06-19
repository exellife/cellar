#!/usr/bin/env bash
# Self-service registration (#51) end-to-end: boot pgforge with the taxi example
# policy (rider/driver self-registerable, admin not) and run register_test.py
# against it. Single-tenant mode (no PGF_TENANT_COLUMN). register_test.sh <binary>
set -euo pipefail

BIN="${1:?usage: register_test.sh <pgforge-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export PGF_POLICY_FILE="$DIR/config/policies.taxi.example.json"
# This test exercises the auto-login opt-out path (201 + token, 409 on duplicate).
# The secure default (uniform 202, no enumeration oracle) is covered separately by
# register_enum_test.sh.
export PGF_REGISTER_AUTOLOGIN=1
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/register_test.py"
