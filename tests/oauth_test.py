#!/usr/bin/env python3
"""pgforge OIDC sign-in end-to-end (#auth, PLAN §7e, "Option B").

Mints real RS256 ID tokens with a throwaway RSA key (signed via the openssl CLI,
no Python crypto lib) whose public half pgforge fetches from a local JWKS server
(see oauth_test.sh). Exercises the whole verify + find-or-link path offline:
auto-provision, replay (existing identity), link-by-verified-email, and the
rejection paths (bad signature, wrong audience, expired, unknown provider).

Env (set by oauth_test.sh): PGF_OAUTH_KEY (private key), PGF_OAUTH_KID, OIDC_ISS,
OIDC_AUD, plus the seeded LINK_EMAIL. The harness passes ws://host:port/ as argv[1].
"""
import base64, hashlib, hmac, json, os, struct, subprocess, sys, time
from urllib.parse import urlparse

KEY = os.environ["PGF_OAUTH_KEY"]
KID = os.environ.get("PGF_OAUTH_KID", "test-key-1")
ISS = os.environ.get("OIDC_ISS", "https://test.issuer")
AUD = os.environ.get("OIDC_AUD", "test-client")
LINK_EMAIL = os.environ.get("LINK_EMAIL", "linkme@test.local")
NEW_EMAIL = os.environ.get("NEW_EMAIL", "newoauth@test.local")
UNTRUSTED_EMAIL = os.environ.get("UNTRUSTED_EMAIL", "untrusted@other.local")
HOST = PORT = None
import http.client


def totp(secret_b32):  # independent RFC-6238 TOTP, mirrors mfa_test.py
    key = base64.b32decode(secret_b32 + "=" * ((8 - len(secret_b32) % 8) % 8))
    counter = struct.pack(">Q", int(time.time()) // 30)
    mac = hmac.new(key, counter, hashlib.sha1).digest()
    off = mac[-1] & 0x0F
    n = ((mac[off] & 0x7F) << 24) | (mac[off + 1] << 16) | (mac[off + 2] << 8) | mac[off + 3]
    return "%06d" % (n % 1000000)


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


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def mint(sub, email, email_verified=True, aud=AUD, exp_delta=300, kid=KID,
         azp=None, nonce=None, iat_delta=0, key=None):
    now = int(time.time())
    header = b64url(json.dumps({"alg": "RS256", "typ": "JWT", "kid": kid}).encode())
    claims = {"iss": ISS, "aud": aud, "sub": sub, "email": email,
              "email_verified": email_verified, "iat": now + iat_delta, "exp": now + exp_delta}
    if azp is not None:   claims["azp"] = azp
    if nonce is not None: claims["nonce"] = nonce
    payload = b64url(json.dumps(claims).encode())
    signing_input = (header + "." + payload).encode()
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key or KEY, "-binary"],
                         input=signing_input, capture_output=True).stdout
    return header + "." + payload + "." + b64url(sig)


def oauth(provider, id_token, nonce=None):
    body = {"provider": provider, "id_token": id_token}
    if nonce is not None: body["nonce"] = nonce
    return req("POST", "/auth/oauth", body)


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<44} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge OIDC sign-in harness -> {HOST}:{PORT} ==")

    # 1. a brand-new verified identity auto-provisions an account + logs in
    s, b = oauth("test", mint("oidc|new-1", NEW_EMAIL))
    tok = (b or {}).get("token")
    chk("auto-provision new user -> token", s == 200 and bool(tok), f"status={s}")
    chk("provisioned user email", (b or {}).get("user", {}).get("email") == NEW_EMAIL)
    s, _ = req("GET", "/schema", token=tok)
    chk("oauth session authenticates", s == 200, f"status={s}")

    # 2. same subject again resolves to the same account (existing identity)
    s, b = oauth("test", mint("oidc|new-1", NEW_EMAIL))
    chk("replay same sub -> same user",
        s == 200 and (b or {}).get("user", {}).get("email") == NEW_EMAIL, f"status={s}")

    # 3. a verified email in a TRUSTED domain links to a pre-existing account
    s, b = oauth("test", mint("oidc|link-1", LINK_EMAIL))
    chk("link by verified email (trusted domain)",
        s == 200 and (b or {}).get("user", {}).get("email") == LINK_EMAIL, f"status={s}")

    # 3b. H-3: an existing account whose email domain is NOT trusted for this
    #     provider must NOT be auto-linked on the email_verified claim alone —
    #     the collision is refused (403), not silently merged (account takeover).
    s, b = oauth("test", mint("oidc|untrusted-1", UNTRUSTED_EMAIL))
    chk("untrusted-domain email NOT linked -> 403", s == 403, f"status={s} body={b}")

    # 3c. H-2: federated login honors the SAME MFA gate as password login. Enroll
    #     TOTP on the OAuth-provisioned user, then signing in via OAuth must return
    #     an mfa_required challenge (no session), completed via /auth/mfa/verify.
    s, b = req("POST", "/auth/mfa/enroll", token=tok)
    secret = (b or {}).get("secret")
    chk("oauth user enroll TOTP -> secret", s == 200 and bool(secret), f"status={s}")
    s, _ = req("POST", "/auth/mfa/confirm", {"code": totp(secret)}, token=tok)
    chk("oauth user confirm TOTP -> 200", s == 200, f"status={s}")
    s, b = oauth("test", mint("oidc|new-1", NEW_EMAIL))
    chk("oauth login now mfa_required (no token)",
        s == 200 and (b or {}).get("status") == "mfa_required"
        and bool((b or {}).get("challenge")) and not (b or {}).get("token"), f"status={s} body={b}")
    s, b = req("POST", "/auth/mfa/verify",
               {"challenge": (b or {}).get("challenge"), "code": totp(secret)})
    chk("oauth mfa verify -> session token", s == 200 and bool((b or {}).get("token")), f"status={s}")

    # ---- rejection paths ----
    # 4. a tampered signature
    bad = mint("oidc|x", NEW_EMAIL)
    bad = bad[:-3] + ("aaa" if bad[-3:] != "aaa" else "bbb")
    s, _ = oauth("test", bad)
    chk("tampered signature -> 401", s == 401, f"status={s}")

    # 5. wrong audience — and L-2: the client gets a GENERIC reason, never the
    #    verification stage ("audience mismatch"), which is an oracle + leaks aud.
    s, b = oauth("test", mint("oidc|x", NEW_EMAIL, aud="someone-else"))
    chk("wrong audience -> 401", s == 401, f"status={s}")
    chk("verify error is generic (L-2)", (b or {}).get("message") == "invalid token",
        repr((b or {}).get("message")))

    # 6. expired token
    s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, exp_delta=-300))
    chk("expired token -> 401", s == 401, f"status={s}")

    # 7. unknown signing key (kid not in JWKS)
    s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, kid="no-such-kid"))
    chk("unknown kid -> 401", s == 401, f"status={s}")

    # 8. unknown provider
    s, _ = oauth("nope", mint("oidc|x", NEW_EMAIL))
    chk("unknown provider -> 401", s == 401, f"status={s}")

    # ---- M-6: azp / iat / nonce ----
    # multi-audience token without an azp claim -> rejected (OIDC 3.1.3.7).
    s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, aud=[AUD, "other-rp"]))
    chk("multi-aud without azp -> 401 (M-6)", s == 401, f"status={s}")
    # multi-aud with azp for a DIFFERENT party -> rejected (cross-RP replay).
    s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, aud=[AUD, "other-rp"], azp="other-rp"))
    chk("azp mismatch -> 401 (M-6)", s == 401, f"status={s}")
    # multi-aud with azp == our client_id -> accepted (existing identity, no MFA).
    s, b = oauth("test", mint("oidc|link-1", LINK_EMAIL, aud=[AUD, "other-rp"], azp=AUD))
    chk("multi-aud + correct azp -> 200 (M-6)", s == 200 and bool((b or {}).get("token")), f"status={s}")
    # future-dated iat (beyond skew) -> rejected.
    s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, iat_delta=3600))
    chk("future-dated iat -> 401 (M-6)", s == 401, f"status={s}")
    # nonce: a supplied nonce must match the token's; mismatch rejected, match ok.
    s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, nonce="server-A"), nonce="server-B")
    chk("nonce mismatch -> 401 (M-6)", s == 401, f"status={s}")
    s, b = oauth("test", mint("oidc|link-1", LINK_EMAIL, nonce="server-A"), nonce="server-A")
    chk("nonce match -> 200 (M-6)", s == 200 and bool((b or {}).get("token")), f"status={s}")

    # ---- L-6: an undersized RSA JWKS key is rejected for RS256 ----
    weak_key = os.environ.get("PGF_OAUTH_WEAK_KEY")
    if weak_key:
        wkid = os.environ.get("WEAK_KID", "weak-1024")
        s, _ = oauth("test", mint("oidc|x", NEW_EMAIL, kid=wkid, key=weak_key))
        chk("undersized (1024-bit) JWKS key -> 401 (L-6)", s == 401, f"status={s}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
