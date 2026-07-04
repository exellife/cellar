#include "password.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>

int cel_crypto_init(void) {
    return sodium_init() < 0 ? -1 : 0;
}

int cel_token_hash(const char *token, char *out, size_t out_size) {
    if (!token || out_size < crypto_hash_sha256_BYTES * 2 + 1) return -1;
    unsigned char h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, (const unsigned char *)token, strlen(token));
    sodium_bin2hex(out, out_size, h, sizeof h);
    return 0;
}

int cel_password_hash(const char *password, char *out, size_t out_size) {
    if (out_size < crypto_pwhash_STRBYTES) return -1;
    /* INTERACTIVE limits: tuned for login latency, still strong. */
    if (crypto_pwhash_str(out, password, strlen(password),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return -1; /* typically out of memory */
    }
    return 0;
}

bool cel_password_verify(const char *hash, const char *password) {
    return crypto_pwhash_str_verify(hash, password, strlen(password)) == 0;
}

int cel_random_token_hex(char *out, size_t out_size, size_t nbytes) {
    unsigned char buf[64];
    if (nbytes > sizeof buf) return -1;
    if (out_size < nbytes * 2 + 1) return -1;
    randombytes_buf(buf, nbytes);
    sodium_bin2hex(out, out_size, buf, nbytes);
    return 0;
}

/* A numeric one-time code of `digits` digits (leading zeros preserved), drawn
 * from a uniform, bias-free CSPRNG. `out` needs >= digits+1 bytes. */
int cel_random_code(char *out, size_t out_size, unsigned digits) {
    if (digits < 4 || digits > 9 || out_size < (size_t)digits + 1) return -1;
    uint32_t bound = 1;
    for (unsigned i = 0; i < digits; i++) bound *= 10u;
    uint32_t n = randombytes_uniform(bound);   /* unbiased 0 .. bound-1 */
    snprintf(out, out_size, "%0*u", (int)digits, n);
    return 0;
}

int cel_uuid_v4(char *out, size_t out_size) {
    if (out_size < 37) return -1;
    uuid_t u;
    uuid_generate_random(u);          /* libuuid; CSPRNG-backed */
    uuid_unparse_lower(u, out);       /* 36 chars + NUL */
    return 0;
}
