#!/usr/bin/env bash
# Rate-limit end-to-end: boot with low limits and verify BOTH the auth throttle
# (/auth/login -> 429) and the data-API throttle (/api/<table> -> 429) kick in,
# from independent buckets. rate_limit_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: rate_limit_test.sh <pgforge-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export PGF_AUTH_RATELIMIT=3/60
export PGF_API_RATELIMIT=3/60
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/rate_limit_check.py"
