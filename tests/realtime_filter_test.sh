#!/usr/bin/env bash
# on_realtime delivery-filter e2e: boot with the on_realtime bundle and verify a
# SILENT-titled note is dropped while normal notes deliver. realtime_filter_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: realtime_filter_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_HOOKS_FILE="$DIR/tests/hooks_realtime.lua"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/realtime_filter.py"
