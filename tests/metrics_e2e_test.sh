#!/usr/bin/env bash
# Observability e2e: /metrics is bearer-gated (L-3), so boot with a token set and
# let metrics_e2e.py scrape with it (and prove it's denied without it).
# metrics_e2e_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: metrics_e2e_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_METRICS_TOKEN="metrics-scrape-secret"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/metrics_e2e.py"
