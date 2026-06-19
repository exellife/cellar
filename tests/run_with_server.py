#!/usr/bin/env python3
"""Boot a pgforge server, run a test script against it, tear it down.

Usage: run_with_server.py <pgforge_binary> <test_script.py>

Picks a free port, seeds an admin, waits for the listener, runs
  python3 <test_script.py> ws://127.0.0.1:<port>/
and forwards the script's exit code. Dumps the server log on failure.
"""
import os, sys, socket, subprocess, time, tempfile

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]; s.close(); return p

def wait_listen(port, proc, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        with socket.socket() as s:
            s.settimeout(0.25)
            try:
                s.connect(("127.0.0.1", port)); return True
            except OSError:
                time.sleep(0.05)
    return False

def main():
    if len(sys.argv) < 3:
        print("usage: run_with_server.py <binary> <test_script>", file=sys.stderr)
        return 2
    binary, script = sys.argv[1], sys.argv[2]
    port = free_port()

    env = dict(os.environ)
    env.update(
        PGF_PORT=str(port),
        PGF_LOG_LEVEL=env.get("PGF_LOG_LEVEL", "warn"),
        PGF_DB_NAME=env.get("PGF_DB_NAME", "pgforge"),
        # Seed users per role (two editors, to exercise row-level ownership).
        PGF_SEED_USERS=env.get("PGF_SEED_USERS",
            "admin@pgforge.dev:s3cret-admin:admin;"
            "editor@pgforge.dev:editor-pw:editor;"
            "editor2@pgforge.dev:editor2-pw:editor;"
            "viewer@pgforge.dev:viewer-pw:viewer"),
        # Policy overrides (row-level ownership for `notes`).
        PGF_POLICY_FILE=env.get("PGF_POLICY_FILE",
            os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "config", "policies.json")),
        PGF_TEST_EMAIL=env.get("PGF_TEST_EMAIL", "admin@pgforge.dev"),
        PGF_TEST_PASSWORD=env.get("PGF_TEST_PASSWORD", "s3cret-admin"),
        # Disable the auth rate limiter by default so functional tests can log in
        # / register freely; the dedicated rate-limit test overrides this.
        PGF_AUTH_RATELIMIT=env.get("PGF_AUTH_RATELIMIT", "0"),
    )

    log = tempfile.NamedTemporaryFile(prefix="pgforge-", suffix=".log", delete=False)
    proc = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        if not wait_listen(port, proc):
            print(f"server failed to listen on :{port}", file=sys.stderr)
            log.flush()
            sys.stderr.write(open(log.name).read())
            return 1
        runner = ["node"] if script.endswith((".mjs", ".js")) else [sys.executable]
        rc = subprocess.call(runner + [script, f"ws://127.0.0.1:{port}/"], env=env)
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        log.flush()

    if rc != 0:
        sys.stderr.write("\n----- server log -----\n")
        sys.stderr.write(open(log.name).read())
    os.unlink(log.name)
    return rc

if __name__ == "__main__":
    sys.exit(main())
