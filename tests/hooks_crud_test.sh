#!/usr/bin/env bash
# CRUD-hook e2e: boot with the before/authorize/after bundle (hooks_crud.lua) and
# exercise the write path. hooks_crud_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: hooks_crud_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_HOOKS_FILE="$DIR/tests/hooks_crud.lua"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/hooks_crud_test.py"
