#include "oauth.h"
#include "logger.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <cjson/cJSON.h>

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_PROVIDERS  8
#define MAX_JWKS_KEYS  16
#define EXP_LEEWAY     60       /* seconds of clock-skew tolerance on exp */
#define JWKS_MIN_REFETCH 30     /* don't refetch a key set more than this often */

/* ---- provider config ------------------------------------------------------- */

typedef struct {
    char name[32];
    char issuer[256];
    char jwks_uri[256];
    char audience[256];          /* the OAuth client id this deployment expects */
    char trusted_domains[256];   /* CSV email domains this provider may auto-link (H-3) */
} provider_t;

static provider_t g_providers[MAX_PROVIDERS];
static int        g_nproviders = 0;

static const provider_t *find_provider(const char *name) {
    for (int i = 0; i < g_nproviders; i++)
        if (!strcmp(g_providers[i].name, name)) return &g_providers[i];
    return NULL;
}

/* Built-in issuer/JWKS for well-known providers (operator still sets client id). */
static void provider_defaults(const char *name, char *issuer, char *jwks) {
    if (!strcmp(name, "google")) {
        snprintf(issuer, 256, "%s", "https://accounts.google.com");
        snprintf(jwks,   256, "%s", "https://www.googleapis.com/oauth2/v3/certs");
    } else if (!strcmp(name, "apple")) {
        snprintf(issuer, 256, "%s", "https://appleid.apple.com");
        snprintf(jwks,   256, "%s", "https://appleid.apple.com/auth/keys");
    }
}

static const char *env_for(const char *name, const char *suffix) {
    char key[96], up[32];
    size_t i = 0;
    for (; name[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)name[i]);
    up[i] = '\0';
    snprintf(key, sizeof key, "PGF_OAUTH_%s_%s", up, suffix);
    return getenv(key);
}

void pgf_oauth_init(void) {
    const char *list = getenv("PGF_OAUTH_PROVIDERS");
    if (!list || !*list) return;

    char *dup = strdup(list);
    if (!dup) return;
    for (char *tok = strtok(dup, ","); tok && g_nproviders < MAX_PROVIDERS; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (!*tok) continue;
        provider_t p;
        memset(&p, 0, sizeof p);
        snprintf(p.name, sizeof p.name, "%s", tok);
        provider_defaults(p.name, p.issuer, p.jwks_uri);

        const char *iss = env_for(tok, "ISSUER");
        const char *jwk = env_for(tok, "JWKS");
        const char *aud = env_for(tok, "CLIENT_ID");
        const char *dom = env_for(tok, "TRUSTED_DOMAINS");   /* H-3: auto-link allow-list */
        if (iss) snprintf(p.issuer, sizeof p.issuer, "%s", iss);
        if (jwk) snprintf(p.jwks_uri, sizeof p.jwks_uri, "%s", jwk);
        if (aud) snprintf(p.audience, sizeof p.audience, "%s", aud);
        if (dom) snprintf(p.trusted_domains, sizeof p.trusted_domains, "%s", dom);

        if (!p.issuer[0] || !p.jwks_uri[0] || !p.audience[0]) {
            LOG_WARN("oauth: provider '%s' incomplete (need issuer, jwks, client id) — skipped", p.name);
            continue;
        }
        g_providers[g_nproviders++] = p;
        LOG_INFO("oauth: provider '%s' configured (iss=%s)", p.name, p.issuer);
    }
    free(dup);
}

bool pgf_oauth_enabled(void) { return g_nproviders > 0; }

/* True iff a federated identity from `provider` may be auto-linked to an existing
 * local account with this `email` — i.e. the email's domain is in the provider's
 * configured PGF_OAUTH_<NAME>_TRUSTED_DOMAINS allow-list. Fails closed: no
 * allow-list (or unknown provider / missing domain) => never link (H-3). This
 * stops a loose or hostile IdP from asserting a victim's "verified" email to take
 * over the account; merging by email is allowed only for domains the operator
 * vouches the provider is authoritative for. Matching is case-insensitive. */
bool pgf_oauth_email_link_allowed(const char *provider, const char *email) {
    if (!provider || !email) return false;
    const char *at = strrchr(email, '@');
    if (!at || !at[1]) return false;
    const char *domain = at + 1;

    const provider_t *p = find_provider(provider);
    if (!p || !p->trusted_domains[0]) return false;

    char dup[256];
    snprintf(dup, sizeof dup, "%s", p->trusted_domains);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t n = strlen(tok);
        while (n > 0 && (tok[n-1] == ' ' || tok[n-1] == '\t')) tok[--n] = '\0';
        if (*tok && !strcasecmp(tok, domain)) return true;
    }
    return false;
}

/* ---- JWKS cache ------------------------------------------------------------ */

typedef struct {
    char       jwks_uri[256];
    char       kids[MAX_JWKS_KEYS][128];
    EVP_PKEY  *keys[MAX_JWKS_KEYS];
    int        nkeys;
    time_t     fetched;
    bool       used;
} jwks_entry_t;

static jwks_entry_t   g_jwks[MAX_PROVIDERS];
static pthread_mutex_t g_jwks_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- base64url ------------------------------------------------------------- */

static int b64url_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int b64url_decode(const char *in, size_t inlen, unsigned char *out, size_t outsz, size_t *outlen) {
    uint32_t buf = 0;
    int bits = 0;
    size_t oi = 0;
    for (size_t i = 0; i < inlen; i++) {
        int v = b64url_val((unsigned char)in[i]);
        if (v < 0) return -1;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            if (oi >= outsz) return -1;
            out[oi++] = (buf >> (bits - 8)) & 0xFF;
            bits -= 8;
        }
    }
    *outlen = oi;
    return 0;
}

/* base64url-decode a JSON-string field into a NUL-terminated text buffer. */
static int b64url_decode_str(const char *in, size_t inlen, char *out, size_t outsz) {
    size_t n = 0;
    if (b64url_decode(in, inlen, (unsigned char *)out, outsz - 1, &n) != 0) return -1;
    out[n] = '\0';
    return 0;
}

/* ---- RSA public key + RS256 verify ----------------------------------------- */

/* Build an EVP_PKEY (RSA public) from base64url modulus/exponent (OpenSSL 3). */
static EVP_PKEY *rsa_pub_from_jwk(const char *n_b64, const char *e_b64) {
    unsigned char nbin[512], ebin[16];
    size_t nlen = 0, elen = 0;
    if (b64url_decode(n_b64, strlen(n_b64), nbin, sizeof nbin, &nlen) != 0 ||
        b64url_decode(e_b64, strlen(e_b64), ebin, sizeof ebin, &elen) != 0)
        return NULL;

    EVP_PKEY *pkey = NULL;
    BIGNUM *bn_n = BN_bin2bn(nbin, (int)nlen, NULL);
    BIGNUM *bn_e = BN_bin2bn(ebin, (int)elen, NULL);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    if (bn_n && bn_e && bld &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e) &&
        (params = OSSL_PARAM_BLD_to_param(bld)) &&
        (ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL)) &&
        EVP_PKEY_fromdata_init(ctx) > 0)
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);

    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_free(bn_n);
    BN_free(bn_e);

    /* L-6: reject undersized RSA keys. Without this floor a 512-/1024-bit key in
     * the JWKS would be accepted for RS256 verification; real providers serve
     * >= 2048-bit, so a smaller key signals a compromised/misconfigured endpoint. */
    if (pkey && EVP_PKEY_bits(pkey) < 2048) {
        EVP_PKEY_free(pkey);
        pkey = NULL;
    }
    return pkey;
}

static bool rs256_verify(EVP_PKEY *pub, const char *signing_input, size_t inlen,
                         const unsigned char *sig, size_t siglen) {
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    bool ok = md &&
        EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pub) == 1 &&
        EVP_DigestVerify(md, sig, siglen, (const unsigned char *)signing_input, inlen) == 1;
    EVP_MD_CTX_free(md);
    return ok;
}

/* ---- JWKS fetch (libcurl) -------------------------------------------------- */

typedef struct { char *buf; size_t len; } curlbuf_t;

static size_t curl_sink(void *data, size_t sz, size_t n, void *ud) {
    curlbuf_t *b = ud;
    size_t add = sz * n;
    if (b->len + add > 1024 * 1024) return 0;   /* 1 MB cap on a JWKS doc */
    char *p = realloc(b->buf, b->len + add + 1);
    if (!p) return 0;
    b->buf = p;
    memcpy(b->buf + b->len, data, add);
    b->len += add;
    b->buf[b->len] = '\0';
    return add;
}

/* GET the JWKS document; caller frees *out. Returns 0 on success. */
static int jwks_fetch(const char *uri, char **out) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curlbuf_t b = {0};
    curl_easy_setopt(c, CURLOPT_URL, uri);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200 || !b.buf) {
        free(b.buf);
        LOG_WARN("oauth: JWKS fetch failed (%s, http=%ld): %s", uri, code, curl_easy_strerror(rc));
        return -1;
    }
    *out = b.buf;
    return 0;
}

static void entry_free_keys(jwks_entry_t *e) {
    for (int i = 0; i < e->nkeys; i++) { EVP_PKEY_free(e->keys[i]); e->keys[i] = NULL; }
    e->nkeys = 0;
}

/* Parse a JWKS JSON doc into `e` (replacing its keys). Caller holds the lock. */
static void jwks_parse_into(jwks_entry_t *e, const char *doc, time_t now) {
    cJSON *root = cJSON_Parse(doc);
    cJSON *keys = root ? cJSON_GetObjectItemCaseSensitive(root, "keys") : NULL;
    entry_free_keys(e);
    if (cJSON_IsArray(keys)) {
        cJSON *k;
        cJSON_ArrayForEach(k, keys) {
            if (e->nkeys >= MAX_JWKS_KEYS) break;
            const cJSON *kty = cJSON_GetObjectItemCaseSensitive(k, "kty");
            const cJSON *kid = cJSON_GetObjectItemCaseSensitive(k, "kid");
            const cJSON *n   = cJSON_GetObjectItemCaseSensitive(k, "n");
            const cJSON *ex  = cJSON_GetObjectItemCaseSensitive(k, "e");
            if (!cJSON_IsString(kty) || strcmp(kty->valuestring, "RSA") != 0) continue;
            if (!cJSON_IsString(kid) || !cJSON_IsString(n) || !cJSON_IsString(ex)) continue;
            EVP_PKEY *pk = rsa_pub_from_jwk(n->valuestring, ex->valuestring);
            if (!pk) continue;
            snprintf(e->kids[e->nkeys], sizeof e->kids[0], "%s", kid->valuestring);
            e->keys[e->nkeys] = pk;
            e->nkeys++;
        }
    }
    e->fetched = now;
    cJSON_Delete(root);
}

/* Return an up-ref'd public key for (jwks_uri, kid); the caller EVP_PKEY_free()s
 * it. Fetches/refreshes the key set on an unknown kid (rate-limited). */
static EVP_PKEY *jwks_get_key(const char *jwks_uri, const char *kid) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_jwks_lock);

    jwks_entry_t *e = NULL, *slot = NULL;
    for (int i = 0; i < MAX_PROVIDERS; i++) {
        if (g_jwks[i].used && !strcmp(g_jwks[i].jwks_uri, jwks_uri)) { e = &g_jwks[i]; break; }
        if (!slot && !g_jwks[i].used) slot = &g_jwks[i];
    }

    for (int round = 0; round < 2; round++) {
        if (e) {
            for (int i = 0; i < e->nkeys; i++)
                if (!strcmp(e->kids[i], kid)) {
                    EVP_PKEY *pk = e->keys[i];
                    EVP_PKEY_up_ref(pk);
                    pthread_mutex_unlock(&g_jwks_lock);
                    return pk;
                }
        }
        /* Unknown kid: refetch once (unless we just did or are rate-limited). */
        if (round == 1) break;
        if (e && now - e->fetched < JWKS_MIN_REFETCH) break;

        pthread_mutex_unlock(&g_jwks_lock);          /* no lock across network I/O */
        char *doc = NULL;
        int frc = jwks_fetch(jwks_uri, &doc);
        pthread_mutex_lock(&g_jwks_lock);
        if (frc != 0 || !doc) { free(doc); break; }

        if (!e) {                                    /* first fetch for this uri */
            e = slot;
            if (!e) { free(doc); break; }            /* cache full */
            memset(e, 0, sizeof *e);
            e->used = true;
            snprintf(e->jwks_uri, sizeof e->jwks_uri, "%s", jwks_uri);
        }
        jwks_parse_into(e, doc, now);
        free(doc);
    }

    pthread_mutex_unlock(&g_jwks_lock);
    return NULL;
}

void pgf_oauth_cleanup(void) {
    pthread_mutex_lock(&g_jwks_lock);
    for (int i = 0; i < MAX_PROVIDERS; i++) if (g_jwks[i].used) entry_free_keys(&g_jwks[i]);
    pthread_mutex_unlock(&g_jwks_lock);
}

/* ---- token verification ---------------------------------------------------- */

#define VFAIL(...) do { snprintf(errbuf, errlen, __VA_ARGS__); goto done; } while (0)

/* Does the token's aud claim contain our expected audience? (aud may be a string
 * or an array of strings.) */
static bool aud_matches(const cJSON *payload, const char *expect) {
    const cJSON *aud = cJSON_GetObjectItemCaseSensitive(payload, "aud");
    if (cJSON_IsString(aud)) return strcmp(aud->valuestring, expect) == 0;
    if (cJSON_IsArray(aud)) {
        const cJSON *a;
        cJSON_ArrayForEach(a, aud)
            if (cJSON_IsString(a) && strcmp(a->valuestring, expect) == 0) return true;
    }
    return false;
}

static bool claim_true(const cJSON *v) {
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strcmp(v->valuestring, "true") == 0;   /* Apple sends a string */
    return false;
}

int pgf_oauth_verify(const char *provider, const char *id_token, const char *expected_nonce,
                     pgf_oauth_claims_t *out, char *errbuf, size_t errlen) {
    errbuf[0] = '\0';
    const provider_t *p = provider ? find_provider(provider) : NULL;
    if (!p) { snprintf(errbuf, errlen, "unknown provider"); return -1; }
    if (!id_token || !*id_token) { snprintf(errbuf, errlen, "missing token"); return -1; }

    int rc = -1;
    cJSON *hdr = NULL, *pl = NULL;
    EVP_PKEY *pk = NULL;
    unsigned char sig[1024];

    /* Split header.payload.signature. */
    const char *d1 = strchr(id_token, '.');
    const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2 || strchr(d2 + 1, '.')) VFAIL("malformed token");
    size_t hlen = (size_t)(d1 - id_token);
    size_t plen = (size_t)(d2 - (d1 + 1));
    size_t slen = strlen(d2 + 1);

    char hbuf[2048], pbuf[8192];
    if (hlen >= sizeof hbuf || plen >= sizeof pbuf) VFAIL("token too large");
    if (b64url_decode_str(id_token, hlen, hbuf, sizeof hbuf) != 0) VFAIL("bad header");
    if (b64url_decode_str(d1 + 1, plen, pbuf, sizeof pbuf) != 0) VFAIL("bad payload");

    hdr = cJSON_Parse(hbuf);
    pl  = cJSON_Parse(pbuf);
    if (!hdr || !pl) VFAIL("bad token json");

    const cJSON *alg = cJSON_GetObjectItemCaseSensitive(hdr, "alg");
    const cJSON *kid = cJSON_GetObjectItemCaseSensitive(hdr, "kid");
    if (!cJSON_IsString(alg) || strcmp(alg->valuestring, "RS256") != 0) VFAIL("unsupported alg");
    if (!cJSON_IsString(kid)) VFAIL("missing kid");

    /* Signature check first — never trust unverified claims. */
    size_t siglen = 0;
    if (b64url_decode(d2 + 1, slen, sig, sizeof sig, &siglen) != 0) VFAIL("bad signature encoding");
    pk = jwks_get_key(p->jwks_uri, kid->valuestring);
    if (!pk) VFAIL("unknown signing key");
    if (!rs256_verify(pk, id_token, hlen + 1 + plen, sig, siglen)) VFAIL("signature invalid");

    /* Now the registered claims. */
    const cJSON *iss = cJSON_GetObjectItemCaseSensitive(pl, "iss");
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(pl, "exp");
    const cJSON *sub = cJSON_GetObjectItemCaseSensitive(pl, "sub");
    if (!cJSON_IsString(iss) || strcmp(iss->valuestring, p->issuer) != 0) VFAIL("issuer mismatch");
    if (!aud_matches(pl, p->audience)) VFAIL("audience mismatch");

    /* azp (OIDC Core 3.1.3.7, M-6): when the token carries multiple audiences an
     * azp (authorized party) claim MUST be present and equal our client_id, and
     * whenever azp is present at all it MUST equal our client_id. Without this a
     * token minted for a DIFFERENT relying party whose aud array also lists our
     * client_id is accepted — cross-relying-party token replay. */
    const cJSON *aud = cJSON_GetObjectItemCaseSensitive(pl, "aud");
    const cJSON *azp = cJSON_GetObjectItemCaseSensitive(pl, "azp");
    if (cJSON_IsArray(aud) && cJSON_GetArraySize(aud) > 1 && !cJSON_IsString(azp))
        VFAIL("azp required for multi-audience token");
    if (cJSON_IsString(azp) && strcmp(azp->valuestring, p->audience) != 0) VFAIL("azp mismatch");

    if (!cJSON_IsNumber(exp) || (time_t)exp->valuedouble + EXP_LEEWAY < time(NULL)) VFAIL("token expired");
    /* iat sanity (M-6): present, numeric, and not future-dated beyond clock skew. */
    const cJSON *iat = cJSON_GetObjectItemCaseSensitive(pl, "iat");
    if (!cJSON_IsNumber(iat) || (time_t)iat->valuedouble - EXP_LEEWAY > time(NULL)) VFAIL("bad iat");
    if (!cJSON_IsString(sub) || !sub->valuestring[0]) VFAIL("missing subject");

    /* Optional nonce binding (M-6): when the caller supplies the nonce it minted
     * for this login, the token's nonce MUST match — defeats verbatim ID-token
     * replay within the exp window. Inert when no nonce is supplied. */
    if (expected_nonce && *expected_nonce) {
        const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(pl, "nonce");
        if (!cJSON_IsString(nonce) || strcmp(nonce->valuestring, expected_nonce) != 0)
            VFAIL("nonce mismatch");
    }

    memset(out, 0, sizeof *out);
    snprintf(out->sub, sizeof out->sub, "%s", sub->valuestring);
    const cJSON *email = cJSON_GetObjectItemCaseSensitive(pl, "email");
    if (cJSON_IsString(email)) snprintf(out->email, sizeof out->email, "%s", email->valuestring);
    out->email_verified = claim_true(cJSON_GetObjectItemCaseSensitive(pl, "email_verified"));
    rc = 0;
done:
    EVP_PKEY_free(pk);
    cJSON_Delete(hdr);
    cJSON_Delete(pl);
    return rc;
}
