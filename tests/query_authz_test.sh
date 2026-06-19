#!/usr/bin/env bash
# Richer-read embedding authz end-to-end: boot pgforge with a policy where
# `categories` is admin-only, then assert that embedding it enforces that policy
# (viewer 403, admin 200). query_authz_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: query_authz_test.sh <pgforge-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export PGF_POLICY_FILE="$DIR/config/policies.embed-authz.example.json"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/query_authz_test.py"
