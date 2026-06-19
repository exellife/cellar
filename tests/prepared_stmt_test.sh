#!/usr/bin/env bash
# Prepared-statement cache e2e: needs /metrics to read the prepare/reuse counters,
# so boot with a metrics token set and let the script scrape with it.
# prepared_stmt_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: prepared_stmt_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_METRICS_TOKEN="metrics-scrape-secret"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/prepared_stmt_test.py"
