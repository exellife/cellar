#!/usr/bin/env bash
# WS keepalive e2e: boot cellar with a fast ping interval and verify portico pings
# idle WebSockets, reaps peers that never PONG, and keeps PONGing peers alive.
# ws_keepalive_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: ws_keepalive_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_WS_PING_INTERVAL=1 CEL_WS_PONG_TIMEOUT=2
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/ws_keepalive.py"
