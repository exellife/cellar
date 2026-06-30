#!/usr/bin/env bash
# Device-token e2e: boot cellar with device tokens enabled (_session.device_ttl_seconds)
# and exercise mint / exchange / list / revoke. device_token_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: device_token_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_POLICY_FILE="$DIR/tests/policies.device-tokens.json"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/device_token_test.py"
