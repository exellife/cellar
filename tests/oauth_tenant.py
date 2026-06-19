#!/usr/bin/env python3
"""cellar M-4: no federated self-provisioning in pooled mode.

Booted with CEL_TENANT_COLUMN set (same JWKS/provider as oauth_test). A brand-new
OAuth identity has no tenant to bind to, so it must be REFUSED (403) rather than
auto-provisioned tenant-less — mirroring /auth/register's pooled-mode block.
Run as: oauth_tenant.py ws://127.0.0.1:<port>/
"""
import base64, http.client, json, os, subprocess, sys, time
from urllib.parse import urlparse

KEY = os.environ["CEL_OAUTH_KEY"]
KID = os.environ.get("CEL_OAUTH_KID", "test-key-1")
ISS = os.environ.get("OIDC_ISS", "https://test.issuer")
AUD = os.environ.get("OIDC_AUD", "test-client")


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def mint(sub, email):
    now = int(time.time())
    h = b64url(json.dumps({"alg": "RS256", "typ": "JWT", "kid": KID}).encode())
    p = b64url(json.dumps({"iss": ISS, "aud": AUD, "sub": sub, "email": email,
                           "email_verified": True, "iat": now, "exp": now + 300}).encode())
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", KEY, "-binary"],
                         input=(h + "." + p).encode(), capture_output=True).stdout
    return h + "." + p + "." + b64url(sig)


def main(host, port):
    c = http.client.HTTPConnection(host, port, timeout=10)
    body = json.dumps({"provider": "test", "id_token": mint("oidc|pooled-new", "pooled-new@test.local")})
    c.request("POST", "/auth/oauth", body=body, headers={"Content-Type": "application/json"})
    r = c.getresponse(); _ = r.read(); c.close()
    print(f"== cellar M-4 pooled-mode OAuth -> {host}:{port} ==")
    ok = (r.status == 403)
    print(f"  {'ok' if ok else 'FAIL'}  new federated user refused in pooled mode "
          f"(no tenant-less provisioning; got {r.status} want 403)")
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    sys.exit(main(u.hostname or "127.0.0.1", u.port or 8080))
