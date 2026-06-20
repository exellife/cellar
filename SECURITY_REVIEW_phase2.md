# Security review — Phase 2 (hook layer + bundle tooling)

A multi-agent adversarial audit of the surface added after the Phase-1 SQLite
pivot: the LuaJIT hook layer, the per-request transaction refactor, and the bundle
tooling (provision / export / import / static serving / control-plane / per-app
policy). Five review dimensions fanned out in parallel; every reported finding was
re-verified by an independent skeptic (default: not-a-bug) before inclusion.

**Result: 4 confirmed, 6 rejected (no false positives). All 4 fixed — commit `3fd7fb6`.**

Hooks are first-party/trusted (not a sandbox), so "a hook can run arbitrary SQL"
is by design; the audit targeted memory safety, correctness, and anything an
ordinary request or a non-malicious operator could trigger.

## Findings & resolution

| ID | Sev | Area | Issue | Fix |
|---|---|---|---|---|
| A1 | **critical** | `import` (main.c) | A symlink planted at `CEL_APPS_DIR/<host>` makes `mkdir` return `EEXIST` (treated as ok), so `tar -xzf -C <target>` extracts **through** the symlink → arbitrary file write under cellar's privileges. | `lstat()` the target; require a real `S_ISDIR` before extracting. |
| A2 | high | `export` (main.c) | `path_exists()` used `stat()` (follows symlinks): a symlinked `<bundle>/data.db` makes `VACUUM INTO` snapshot **another app's** DB → cross-app read. | `lstat()` + require `S_ISREG`. |
| A3 | high | `write_if_absent` (main.c) | `stat()`→`fopen()` TOCTOU: a symlink planted at a bundle file (`hooks.lua`/`index.html`) between the check and the write redirects `provision`'s write. | Single atomic `open(O_WRONLY\|O_CREAT\|O_EXCL)` — refuses to follow a link, no window. |
| B1 | high | `cellar.query` (cel_hooks.c prelude) | The owned `cel_val` result leaks if `deep()` throws mid-copy (e.g. OOM on a large result set) before `cel_val_free(res)`. | Run the copy under `pcall` so the C result is always freed. |

A1–A3 share one root cause: **the bundle CLIs used symlink-following / check-then-use
syscalls on paths under `CEL_APPS_DIR`.** They assume `CEL_APPS_DIR` is
attacker-writable — a defense-in-depth concern (an unprivileged user who can write
the apps dir tricking a privileged operator's CLI into reading/writing outside a
bundle), not a remote vuln. Fixed structurally (lstat-and-reject-symlinks + atomic
`O_EXCL`); the operator should still own/permission the apps dir tightly.

Regression test `cli_symlink` plants each symlink and asserts the exploit is
blocked while legit paths still work.

## What was checked and found clean (rejected findings)

The verifiers rejected 6 reported issues after reading the code, including: the
transaction/connection lifecycle in `run_write` (every exit path releases the
write lock + connection and COMMIT/ROLLBACKs; cJSON freed on all branches);
thread-local request bindings (`app_db` / catalog / policy / hooks cleared by
`cel_apps_leave` on every path); `cel_realtime_publish`'s restructured candidate
collection (app-match still enforced, bounded, fail-closed); Host normalization
and static-docroot containment; and `cel_control`'s use of bound parameters.

## Trust-boundary note (defense in depth)

`CEL_APPS_DIR` (and `CEL_CONTROL_DB`) hold the apps and the registry; treat them
like a service data directory — owned by the cellar service user, not
world-writable. The A1–A3 fixes harden against a compromised apps dir, but tight
directory ownership/permissions remain the primary boundary.
