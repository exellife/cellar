#!/usr/bin/env python3
"""Boot a cellar server, run a test script against it, tear it down.

Usage: run_with_server.py <cellar_binary> <test_script.py>

Picks a free port, seeds an admin, waits for the listener, runs
  python3 <test_script.py> ws://127.0.0.1:<port>/
and forwards the script's exit code. Dumps the server log on failure.
"""
import os, sys, socket, subprocess, time, tempfile, sqlite3, shutil

def make_app_db():
    """Create a throwaway per-app SQLite database with the demo user tables the
    server-boot tests use (the engine applies the cel_* identity schema itself).
    Returns (dir, db_path)."""
    d = tempfile.mkdtemp(prefix="cellar-app-")
    con = sqlite3.connect(os.path.join(d, "data.db"))
    con.executescript(
        "CREATE TABLE IF NOT EXISTS notes("
        "  id INTEGER PRIMARY KEY, owner_id TEXT, title TEXT);"
        "CREATE TABLE IF NOT EXISTS products("
        "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, price REAL);")
    con.close()
    return d, os.path.join(d, "data.db")

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
    app_dir, app_db = make_app_db()

    env = dict(os.environ)
    env.update(
        CEL_PORT=str(port),
        CEL_LOG_LEVEL=env.get("CEL_LOG_LEVEL", "warn"),
        CEL_DATA_DB=env.get("CEL_DATA_DB", app_db),
        # Seed users per role (two editors, to exercise row-level ownership).
        CEL_SEED_USERS=env.get("CEL_SEED_USERS",
            "admin@cellar.dev:s3cret-admin:admin;"
            "editor@cellar.dev:editor-pw:editor;"
            "editor2@cellar.dev:editor2-pw:editor;"
            "viewer@cellar.dev:viewer-pw:viewer"),
        # Policy overrides (row-level ownership for `notes`).
        CEL_POLICY_FILE=env.get("CEL_POLICY_FILE",
            os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "config", "policies.json")),
        CEL_TEST_EMAIL=env.get("CEL_TEST_EMAIL", "admin@cellar.dev"),
        CEL_TEST_PASSWORD=env.get("CEL_TEST_PASSWORD", "s3cret-admin"),
        # Disable the auth rate limiter by default so functional tests can log in
        # / register freely; the dedicated rate-limit test overrides this.
        CEL_AUTH_RATELIMIT=env.get("CEL_AUTH_RATELIMIT", "0"),
    )

    log = tempfile.NamedTemporaryFile(prefix="cellar-", suffix=".log", delete=False)
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
    shutil.rmtree(app_dir, ignore_errors=True)
    return rc

if __name__ == "__main__":
    sys.exit(main())
