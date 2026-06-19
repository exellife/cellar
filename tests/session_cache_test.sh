#!/usr/bin/env bash
# Session cache (#55) end-to-end: boot with CEL_SESSION_CACHE_TTL>0 and prove a
# token whose session row was deleted still resolves (served from cache).
# session_cache_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: session_cache_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_SESSION_CACHE_TTL=60
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/session_cache_check.py"
