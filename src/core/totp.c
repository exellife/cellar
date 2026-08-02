#include "totp.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <sodium.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define TOTP_STEP   30
#define TOTP_DIGITS 6

static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/* base32 (RFC 4648) encode, no padding. NUL-terminates `out`. 0 on success. */
static int b32_encode(const uint8_t *in, size_t inlen, char *out, size_t outsz) {
    size_t need = (inlen * 8 + 4) / 5;
    if (outsz < need + 1) return -1;
    size_t oi = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5) { out[oi++] = B32[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) out[oi++] = B32[(buf << (5 - bits)) & 31];
    out[oi] = '\0';
    return 0;
}

/* base32 decode (case-insensitive; spaces, dashes and '=' padding ignored).
 * Writes raw bytes to `out`, length to `*outlen`. 0 on success, -1 on bad char. */
static int b32_decode(const char *in, uint8_t *out, size_t outsz, size_t *outlen) {
    uint32_t buf = 0;
    int bits = 0;
    size_t oi = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '-' || c == '=') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        const char *pos = memchr(B32, c, 32);
        if (!pos) return -1;
        buf = (buf << 5) | (uint32_t)(pos - B32);
        bits += 5;
        if (bits >= 8) {
            if (oi >= outsz) return -1;
            out[oi++] = (buf >> (bits - 8)) & 0xFF;
            bits -= 8;
        }
    }
    *outlen = oi;
    return 0;
}

/* HOTP (RFC 4226): truncate HMAC-SHA1(key, counter) to TOTP_DIGITS digits. */
static int hotp(const uint8_t *key, size_t keylen, uint64_t counter,
                char *out, size_t outsz) {
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) { msg[i] = (uint8_t)(counter & 0xFF); counter >>= 8; }

    uint8_t mac[EVP_MAX_MD_SIZE];
    unsigned int maclen = 0;
    if (!HMAC(EVP_sha1(), key, (int)keylen, msg, sizeof msg, mac, &maclen) || maclen < 20)
        return -1;

    int off = mac[maclen - 1] & 0x0F;                       /* dynamic truncation */
    uint32_t bin = ((uint32_t)(mac[off]     & 0x7F) << 24)
                 | ((uint32_t)(mac[off + 1] & 0xFF) << 16)
                 | ((uint32_t)(mac[off + 2] & 0xFF) << 8)
                 | ((uint32_t)(mac[off + 3] & 0xFF));
    uint32_t code = bin % 1000000u;                          /* 6 digits */
    if (outsz < TOTP_DIGITS + 1) return -1;
    snprintf(out, outsz, "%06u", code);
    return 0;
}

int cel_totp_code_at(const char *secret_b32, uint64_t unix_time,
                     char *out, size_t out_size) {
    if (!secret_b32) return -1;
    uint8_t key[64];
    size_t keylen = 0;
    if (b32_decode(secret_b32, key, sizeof key, &keylen) != 0 || keylen == 0) return -1;
    int rc = hotp(key, keylen, unix_time / TOTP_STEP, out, out_size);
    sodium_memzero(key, sizeof key);
    return rc;
}

int64_t cel_totp_verify_step(const char *secret_b32, const char *code, int window) {
    if (!secret_b32 || !code || window < 0) return -1;

    char norm[16];                                           /* strip spaces */
    size_t n = 0;
    for (const char *p = code; *p && n < sizeof norm - 1; p++)
        if (*p != ' ') norm[n++] = *p;
    norm[n] = '\0';
    if (n != TOTP_DIGITS) return -1;

    uint64_t now = (uint64_t)time(NULL);
    int64_t matched = -1;
    for (int w = -window; w <= window; w++) {                /* full sweep: constant work */
        uint64_t t = (uint64_t)((int64_t)now + (int64_t)w * TOTP_STEP);
        char expect[8];
        if (cel_totp_code_at(secret_b32, t, expect, sizeof expect) == 0 &&
            sodium_memcmp(expect, norm, TOTP_DIGITS) == 0)
            matched = (int64_t)(t / TOTP_STEP);   /* the code's own step (unique per code) */
        sodium_memzero(expect, sizeof expect);
    }
    return matched;
}

bool cel_totp_verify(const char *secret_b32, const char *code, int window) {
    return cel_totp_verify_step(secret_b32, code, window) >= 0;
}

int cel_totp_generate_secret(char *out, size_t out_size) {
    uint8_t raw[20];                                         /* 160-bit, the RFC default */
    randombytes_buf(raw, sizeof raw);
    int rc = b32_encode(raw, sizeof raw, out, out_size);
    sodium_memzero(raw, sizeof raw);
    return rc;
}

int cel_totp_uri(const char *secret_b32, const char *issuer, const char *account,
                 char *out, size_t out_size) {
    if (!secret_b32 || !issuer || !account) return -1;
    int n = snprintf(out, out_size,
                     "otpauth://totp/%s:%s?secret=%s&issuer=%s&algorithm=SHA1&digits=6&period=30",
                     issuer, account, secret_b32, issuer);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}
