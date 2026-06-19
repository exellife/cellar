#!/usr/bin/env bash
# OIDC sign-in end-to-end: stand up a throwaway RSA key + a local JWKS server,
# point a "test" OAuth provider at it, then run the verify + find-or-link flow
# entirely offline. oauth_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: oauth_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"

TMP="$(mktemp -d)"
KID="test-key-1"
ISS="https://test.issuer"
AUD="test-client"
LINK_EMAIL="oauthlink@test.local"             # domain test.local IS trusted -> may link
NEW_EMAIL="oauthnew@test.local"
UNTRUSTED_EMAIL="oauthuntrusted@other.local"  # other.local NOT trusted -> link refused (H-3)

# Start clean and clean up afterwards (shared DB; identities cascade on user delete).
clean_db() {
  psql -h "$H" -U "$U" -d "$DB" -c \
    "DELETE FROM cel_users WHERE email IN ('$LINK_EMAIL','$NEW_EMAIL','$UNTRUSTED_EMAIL','pooled-new@test.local')" >/dev/null 2>&1 || true
}
clean_db

b64url_mod() { python3 -c "import base64,sys; print(base64.urlsafe_b64encode(bytes.fromhex(sys.argv[1])).rstrip(b'=').decode())" "$1"; }

# Throwaway RSA keypair; publish its public half as a JWKS (e=AQAB = 65537).
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$TMP/key.pem" 2>/dev/null
N="$(b64url_mod "$(openssl rsa -in "$TMP/key.pem" -noout -modulus 2>/dev/null | cut -d= -f2)")"
# Plus an UNDERSIZED 1024-bit key, also published — cellar must refuse it (L-6).
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024 -out "$TMP/key1024.pem" 2>/dev/null
N2="$(b64url_mod "$(openssl rsa -in "$TMP/key1024.pem" -noout -modulus 2>/dev/null | cut -d= -f2)")"
printf '{"keys":[{"kty":"RSA","use":"sig","alg":"RS256","kid":"%s","n":"%s","e":"AQAB"},{"kty":"RSA","use":"sig","alg":"RS256","kid":"weak-1024","n":"%s","e":"AQAB"}]}\n' "$KID" "$N" "$N2" > "$TMP/jwks.json"
export CEL_OAUTH_WEAK_KEY="$TMP/key1024.pem" WEAK_KID="weak-1024"

JPORT="$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")"
( cd "$TMP" && exec python3 -m http.server "$JPORT" --bind 127.0.0.1 ) >/dev/null 2>&1 &
JWKS_PID=$!
trap 'kill $JWKS_PID 2>/dev/null; rm -rf "$TMP"; clean_db' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$JPORT/jwks.json" && break; sleep 0.1; done

# Configure the "test" provider to verify against our local JWKS, plus a policy
# with a self-registerable role so OAuth auto-provisioning has a default role.
export CEL_OAUTH_PROVIDERS="test"
export CEL_OAUTH_TEST_ISSUER="$ISS"
export CEL_OAUTH_TEST_CLIENT_ID="$AUD"
export CEL_OAUTH_TEST_JWKS="http://127.0.0.1:$JPORT/jwks.json"
export CEL_OAUTH_TEST_TRUSTED_DOMAINS="test.local"   # H-3: only this domain may auto-link
export CEL_MFA=optional                               # H-2: enable TOTP so OAuth honors it
export CEL_POLICY_FILE="$DIR/config/policies.taxi.example.json"
export CEL_SEED_USERS="admin@cellar.dev:s3cret-admin:admin;$LINK_EMAIL:linkpw:rider;$UNTRUSTED_EMAIL:untrustedpw:rider"
# token-minting inputs for the python side
export CEL_OAUTH_KEY="$TMP/key.pem" CEL_OAUTH_KID="$KID" OIDC_ISS="$ISS" OIDC_AUD="$AUD"
export LINK_EMAIL NEW_EMAIL UNTRUSTED_EMAIL

# Not exec: keep the EXIT trap so the JWKS server + temp dir + db rows are cleaned.
rc=0
python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/oauth_test.py" || rc=$?
# M-4: re-boot in POOLED mode (same JWKS still serving) and verify a brand-new
# federated identity is refused rather than auto-provisioned tenant-less.
CEL_TENANT_COLUMN=tenant_id \
  python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/oauth_tenant.py" || rc=$?
exit $rc
