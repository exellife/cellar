#!/usr/bin/env bash
# Security regression (Phase-2 audit): the bundle CLIs must not follow symlinks
# planted under CEL_APPS_DIR, so they can't be tricked into reading/writing files
# outside a bundle. cli_symlink_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: cli_symlink_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"; VICTIM="$(mktemp -d)"
trap 'rm -rf "$APPS" "$VICTIM" /tmp/cli_symlink_$$.tar.gz /tmp/cli_symlink_stolen_$$.tar.gz' EXIT
export CEL_LOG_LEVEL=error

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }

# a real archive to attempt imports with
CEL_APPS_DIR="$APPS" "$BIN" provision real.example admin@r secret123 >/dev/null 2>&1
CEL_APPS_DIR="$APPS" "$BIN" export real.example /tmp/cli_symlink_$$.tar.gz >/dev/null 2>&1

# A1 (critical): import into a SYMLINKED target must be refused (no extract through it)
ln -s "$VICTIM" "$APPS/evil.example"
CEL_APPS_DIR="$APPS" "$BIN" import evil.example /tmp/cli_symlink_$$.tar.gz >/dev/null 2>&1 || true
chk "import refuses a symlinked target dir" "$([ -f "$VICTIM/data.db" ] && echo leaked || echo blocked)" "blocked"

# A2 (high): export of an app whose data.db is a SYMLINK must be refused (no cross-app read)
mkdir -p "$APPS/sneaky.example"
ln -s "$APPS/real.example/data.db" "$APPS/sneaky.example/data.db"
CEL_APPS_DIR="$APPS" "$BIN" export sneaky.example /tmp/cli_symlink_stolen_$$.tar.gz >/dev/null 2>&1 || true
chk "export refuses a symlinked data.db" "$([ -f /tmp/cli_symlink_stolen_$$.tar.gz ] && echo leaked || echo blocked)" "blocked"

# A3 (high): provision must NOT follow a pre-planted symlink at a bundle file
mkdir -p "$APPS/t.example"; echo "ORIGINAL" > "$VICTIM/secret.txt"
ln -s "$VICTIM/secret.txt" "$APPS/t.example/hooks.lua"
CEL_APPS_DIR="$APPS" "$BIN" provision t.example admin@t secret123 >/dev/null 2>&1 || true
chk "provision does not follow a symlinked bundle file" \
    "$(grep -q ORIGINAL "$VICTIM/secret.txt" && echo intact || echo clobbered)" "intact"

# sanity: the legit paths still work (a real bundle imports fine)
CEL_APPS_DIR="$APPS" "$BIN" import clone.example /tmp/cli_symlink_$$.tar.gz >/dev/null 2>&1
chk "legit import still works" "$([ -f "$APPS/clone.example/data.db" ] && echo yes || echo no)" "yes"

[ "$fail" = 0 ] && echo "CLI SYMLINK PASS" || { echo "CLI SYMLINK FAIL"; exit 1; }
