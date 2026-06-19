#!/usr/bin/env python3
"""cellar TOTP 2FA end-to-end (#auth, PLAN §7e). Drives the full flow over REST:
enroll -> confirm -> two-step login (challenge + code) -> disable, plus the failure
paths (wrong code 401). Codes are computed here with an INDEPENDENT Python TOTP
(hmac-sha1), so a pass also cross-validates the C RFC-6238 implementation.

Booted by mfa_test.sh with CEL_MFA=optional. The harness passes ws://host:port/.
"""
import base64, hashlib, hmac, http.client, json, struct, sys, time
from urllib.parse import urlparse

USER = ("mfatest@cellar.dev", "mfatest-pw")
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
    return req("POST", "/auth/login", {"email": USER[0], "password": USER[1]})


def totp(secret_b32):
    key = base64.b32decode(secret_b32 + "=" * ((8 - len(secret_b32) % 8) % 8))
    counter = struct.pack(">Q", int(time.time()) // 30)
    mac = hmac.new(key, counter, hashlib.sha1).digest()
    off = mac[-1] & 0x0F
    n = ((mac[off] & 0x7F) << 24) | (mac[off + 1] << 16) | (mac[off + 2] << 8) | mac[off + 3]
    return "%06d" % (n % 1000000)


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<42} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== cellar TOTP 2FA harness -> {HOST}:{PORT} ==")

    # 1. before enrollment, password login returns a session as usual
    s, b = login()
    token = (b or {}).get("token")
    chk("pre-enroll login -> token", s == 200 and bool(token), f"status={s}")

    # 2. enroll: get a secret + otpauth URI
    s, b = req("POST", "/auth/mfa/enroll", token=token)
    secret = (b or {}).get("secret")
    chk("enroll -> secret + otpauth uri",
        s == 200 and bool(secret) and "otpauth://totp/" in (b or {}).get("otpauth_uri", ""),
        f"status={s}")

    # wrong code can't confirm
    s, _ = req("POST", "/auth/mfa/confirm", {"code": "000000"}, token=token)
    chk("confirm wrong code -> 401", s == 401, f"status={s}")

    # 3. confirm with a real current code; it returns one-time recovery codes
    s, b = req("POST", "/auth/mfa/confirm", {"code": totp(secret)}, token=token)
    codes = (b or {}).get("recovery_codes") or []
    chk("confirm correct code -> 200", s == 200, f"status={s}")
    chk("confirm returns 10 recovery codes", len(codes) == 10, f"n={len(codes)}")
    # M-10: each recovery code carries 128 bits (32 hex chars once dashes stripped).
    chk("recovery codes are 128-bit", all(len(c.replace("-", "")) == 32 for c in codes),
        str([len(c.replace("-", "")) for c in codes[:3]]))

    # 4. now password login no longer issues a session — it returns a challenge
    s, b = login()
    challenge = (b or {}).get("challenge")
    chk("login now mfa_required (no token)",
        s == 200 and (b or {}).get("status") == "mfa_required"
        and bool(challenge) and not (b or {}).get("token"), f"status={s}")

    # 5. a wrong code at verify is rejected (and does not burn the challenge)
    wrong = "%06d" % ((int(totp(secret)) + 1) % 1000000)
    s, _ = req("POST", "/auth/mfa/verify", {"challenge": challenge, "code": wrong})
    chk("verify wrong code -> 401", s == 401, f"status={s}")

    # 6. the correct code completes login -> a real session token
    s, b = req("POST", "/auth/mfa/verify", {"challenge": challenge, "code": totp(secret)})
    vtoken = (b or {}).get("token")
    chk("verify correct code -> token", s == 200 and bool(vtoken), f"status={s}")
    s, _ = req("GET", "/schema", token=vtoken)
    chk("mfa-issued session authenticates", s == 200, f"status={s}")

    # 7. the challenge is single-use (already consumed -> 401)
    s, _ = req("POST", "/auth/mfa/verify", {"challenge": challenge, "code": totp(secret)})
    chk("challenge is single-use -> 401", s == 401, f"status={s}")

    # 8. a RECOVERY code works as the second factor (lost-authenticator path)
    ch = (login()[1] or {}).get("challenge")
    s, b = req("POST", "/auth/mfa/verify", {"challenge": ch, "code": codes[0]})
    chk("recovery code completes login", s == 200 and bool((b or {}).get("token")), f"status={s}")
    # that recovery code is single-use
    ch = (login()[1] or {}).get("challenge")
    s, _ = req("POST", "/auth/mfa/verify", {"challenge": ch, "code": codes[0]})
    chk("used recovery code -> 401", s == 401, f"status={s}")
    # a different recovery code still works (same challenge, under the attempt cap)
    s, _ = req("POST", "/auth/mfa/verify", {"challenge": ch, "code": codes[1]})
    chk("a second recovery code works", s == 200, f"status={s}")

    # 9. regenerate (needs a current TOTP code) -> new set; old unused codes die
    s, b = req("POST", "/auth/mfa/recovery-codes", {"code": totp(secret)}, token=vtoken)
    newcodes = (b or {}).get("recovery_codes") or []
    chk("regenerate -> new codes", s == 200 and len(newcodes) == 10, f"status={s}")
    ch = (login()[1] or {}).get("challenge")
    s, _ = req("POST", "/auth/mfa/verify", {"challenge": ch, "code": codes[2]})
    chk("old code after regenerate -> 401", s == 401, f"status={s}")
    s, _ = req("POST", "/auth/mfa/verify", {"challenge": ch, "code": newcodes[0]})
    chk("a regenerated code works", s == 200, f"status={s}")

    # 10. disable requires a valid current code, then login is single-factor again
    s, _ = req("POST", "/auth/mfa/disable", {"code": totp(secret)}, token=vtoken)
    chk("disable -> 200", s == 200, f"status={s}")
    s, b = login()
    chk("login after disable -> token", s == 200 and bool((b or {}).get("token")), f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
