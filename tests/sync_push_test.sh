#!/usr/bin/env bash
# sync_push e2e: boot with a resolve() bundle (sync_push.lua) so the test can cover
# both LWW and the resolve override. sync_push_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: sync_push_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_HOOKS_FILE="$DIR/tests/sync_push.lua"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/sync_push_test.py"
