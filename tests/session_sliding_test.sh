#!/usr/bin/env bash
# Per-app sliding session policy e2e: boot cellar with a sliding _session (short
# idle window + absolute cap) and exercise renew-on-use / cap / idle-drop.
# session_sliding_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: session_sliding_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_POLICY_FILE="$DIR/tests/policies.session-sliding.json"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/session_sliding_test.py"
