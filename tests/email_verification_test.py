#!/usr/bin/env python3
"""pgforge email-verification end-to-end (#auth, PLAN §7e).

Registers a user (which emails a verification token, captured from a local mock
SMTP sink), confirms email_verified starts false, redeems the token, and confirms
it flips to true. Also checks single-use, a bad token, and resend.

Env (set by email_verification_test.sh): PGF_MAIL_CAPTURE, VERIFY_EMAIL.
The harness passes ws://host:port/ as argv[1].
"""
import http.client, json, os, re, sys, time
from urllib.parse import urlparse

EMAIL = os.environ.get("VERIFY_EMAIL", "verifyme@test.local")
PW = "verify-pw-12345"
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


def login():
    return req("POST", "/auth/login", {"email": EMAIL, "password": PW})


def read_verify_token():
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
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<44} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge email-verification harness -> {HOST}:{PORT} ==")

    # register (default self-register role) -> 201, auto-logged-in, email unverified
    s, b = req("POST", "/auth/register", {"email": EMAIL, "password": PW})
    token = (b or {}).get("token")
    chk("register -> 201 + token", s == 201 and bool(token), f"status={s}")
    chk("registered user is unverified", (b or {}).get("user", {}).get("email_verified") is False)

    vtoken = read_verify_token()
    chk("verification token delivered by email", bool(vtoken), vtoken or "(none)")

    # login confirms the flag is false before verifying
    s, b = login()
    chk("login shows email_verified false",
        s == 200 and (b or {}).get("user", {}).get("email_verified") is False, f"status={s}")

    # a bad token is rejected
    s, _ = req("POST", "/auth/verify-email", {"token": "deadbeef"})
    chk("verify bad token -> 400", s == 400, f"status={s}")

    # redeem the real token
    s, _ = req("POST", "/auth/verify-email", {"token": vtoken})
    chk("verify -> 200", s == 200, f"status={s}")

    # now the flag is true
    s, b = login()
    chk("login shows email_verified true",
        s == 200 and (b or {}).get("user", {}).get("email_verified") is True, f"status={s}")

    # the token is single-use
    s, _ = req("POST", "/auth/verify-email", {"token": vtoken})
    chk("reused token -> 400", s == 400, f"status={s}")

    # resend for an already-verified user is a no-op 200 (idempotent)
    s, _ = req("POST", "/auth/verify-email/resend", token=token)
    chk("resend (already verified) -> 200", s == 200, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
