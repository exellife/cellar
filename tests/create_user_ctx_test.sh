#!/usr/bin/env bash
# cellar.create_user context-guard e2e: boot with a bundle whose before() calls
# create_user (must be refused under the write lock, not deadlock) + an rpc that
# calls it safely. create_user_ctx_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: create_user_ctx_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

export CEL_HOOKS_FILE="$DIR/tests/hooks_create_user_ctx.lua"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/create_user_ctx_test.py"
