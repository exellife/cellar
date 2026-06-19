#!/usr/bin/env python3
"""pgforge password-reset end-to-end (#auth, PLAN §7e).

Drives the full flow over REST: forgot-password emails a token (captured from a
local mock SMTP sink), the token is redeemed to set a new password, and then the
new password works / the old one and the old session don't. Also checks the
token is single-use and that forgot is anti-enumeration (always 200).

Env (set by password_reset_test.sh): PGF_MAIL_CAPTURE (the captured email file),
RESET_USER. The harness passes ws://host:port/ as argv[1].
"""
import http.client, json, os, re, sys, time
from urllib.parse import urlparse

USER = os.environ.get("RESET_USER", "pwreset@test.local")
OLD_PW = "oldpassword1"
NEW_PW = "brand-new-pw-9"
CAPTURE = os.environ["PGF_MAIL_CAPTURE"]
HOST = PORT = None


def req(method, path, body=None, token=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    headers = {}
    if body is not None: headers["Content-Type"] = "application/json"
    if token: headers["Authorization"] = "Bearer " + token
    c.request(method, path, body=(json.dumps(body) if body is not None else None), headers=headers)
    r = c.getresponse(); data = r.read(); c.close()
    try: parsed = json.loads(data)
    except Exception: parsed = None
    return r.status, parsed


def login(pw):
    return req("POST", "/auth/login", {"email": USER, "password": pw})


def read_reset_token():
    # forgot returned 200 only after the mail was sent synchronously; the sink has
    # written the captured message by now.
    for _ in range(30):
        if os.path.exists(CAPTURE) and os.path.getsize(CAPTURE) > 0:
            break
        time.sleep(0.1)
    body = open(CAPTURE, "r", errors="replace").read()
    m = re.search(r"token=([0-9a-fA-F]+)", body)
    return m.group(1) if m else None


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<42} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge password-reset harness -> {HOST}:{PORT} ==")

    # baseline: the seeded password works; keep a live session to prove revocation
    s, b = login(OLD_PW)
    old_session = (b or {}).get("token")
    chk("baseline login (old password)", s == 200 and bool(old_session), f"status={s}")

    # L-5: lock the account (3 failed logins). The reset below must CLEAR the
    # lockout — otherwise the new password is refused too and "reset to regain
    # access" silently fails for a locked user.
    for _ in range(3):
        login("wrong-password")
    chk("account locked (correct old pw refused)", login(OLD_PW)[0] == 401, "pre-reset lock")

    # request a reset -> always 200, and an email is sent
    s, _ = req("POST", "/auth/password/forgot", {"email": USER})
    chk("forgot -> 200", s == 200, f"status={s}")

    token = read_reset_token()
    chk("reset token delivered by email", bool(token), token or "(none)")

    # a weak new password is rejected
    s, _ = req("POST", "/auth/password/reset", {"token": token, "password": "short"})
    chk("reset weak password -> 400", s == 400, f"status={s}")

    # redeem the token with a strong password
    s, _ = req("POST", "/auth/password/reset", {"token": token, "password": NEW_PW})
    chk("reset -> 200", s == 200, f"status={s}")

    # the new password works (and the lockout was cleared by the reset — L-5);
    # the old one no longer does
    chk("login new password -> 200 (lockout cleared, L-5)", login(NEW_PW)[0] == 200)
    chk("login old password -> 401", login(OLD_PW)[0] == 401)

    # the reset revoked existing sessions
    s, _ = req("GET", "/schema", token=old_session)
    chk("old session revoked -> 401", s == 401, f"status={s}")

    # the token is single-use
    s, _ = req("POST", "/auth/password/reset", {"token": token, "password": "another-pw-1"})
    chk("reused token -> 400", s == 400, f"status={s}")

    # forgot for an unknown email still returns 200 (no enumeration oracle)
    s, _ = req("POST", "/auth/password/forgot", {"email": "nobody-here@test.local"})
    chk("forgot unknown email -> 200", s == 200, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
