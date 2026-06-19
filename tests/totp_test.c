/* cellar — TOTP (RFC 6238) unit test, no DB.
 *
 * The core correctness proof is the official RFC 6238 Appendix-B test vectors
 * (SHA1, seed = ASCII "12345678901234567890" = base32 GEZDGNBVGY3TQOJQ...). The
 * RFC tabulates 8-digit codes; our 6-digit code is their last 6 digits (mod 10^6
 * == last six decimal digits). If HMAC-SHA1, base32 decode, the counter, or the
 * dynamic truncation were wrong, these would not match.
 */
#include "totp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

int main(void) {
    printf("TOTP (RFC 6238)\n");

    /* base32 of ASCII "12345678901234567890" */
    const char *SECRET = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    struct { unsigned long long t; const char *code6; } vec[] = {
        {59ULL,          "287082"},   /* RFC 8-digit 94287082 */
        {1111111109ULL,  "081804"},   /*            07081804 */
        {1111111111ULL,  "050471"},   /*            14050471 */
        {1234567890ULL,  "005924"},   /*            89005924 */
        {2000000000ULL,  "279037"},   /*            69279037 */
        {20000000000ULL, "353130"},   /*            65353130 */
    };
    for (size_t i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        char out[8] = {0};
        int rc = cel_totp_code_at(SECRET, vec[i].t, out, sizeof out);
        char label[64];
        snprintf(label, sizeof label, "vector t=%llu -> %s", vec[i].t, vec[i].code6);
        chk(label, rc == 0 && strcmp(out, vec[i].code6) == 0);
        if (rc != 0 || strcmp(out, vec[i].code6)) printf("       got '%s'\n", out);
    }

    /* a freshly generated secret is 32 base32 chars from the alphabet */
    char secret[64] = {0};
    chk("generate secret ok", cel_totp_generate_secret(secret, sizeof secret) == 0);
    chk("secret length 32", strlen(secret) == 32);
    int alpha_ok = 1;
    for (const char *p = secret; *p; p++)
        if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", *p)) alpha_ok = 0;
    chk("secret is base32", alpha_ok);

    /* round-trip against the real clock: the current code verifies (window 1),
     * and a guaranteed-different code does not */
    char now_code[8] = {0};
    chk("code_at now ok", cel_totp_code_at(secret, (uint64_t)time(NULL), now_code, sizeof now_code) == 0);
    chk("verify accepts current code", cel_totp_verify(secret, now_code, 1));
    char wrong[8];
    snprintf(wrong, sizeof wrong, "%06d", (atoi(now_code) + 1) % 1000000);
    chk("verify rejects a different code", !cel_totp_verify(secret, wrong, 1));

    /* malformed inputs are always rejected */
    chk("verify rejects short code", !cel_totp_verify(secret, "123", 1));
    chk("verify rejects letters",    !cel_totp_verify(secret, "abcdef", 1));

    /* otpauth URI shape */
    char uri[256];
    chk("uri builds", cel_totp_uri(SECRET, "cellar", "a@b.co", uri, sizeof uri) == 0);
    chk("uri has scheme + secret",
        strncmp(uri, "otpauth://totp/", 15) == 0 && strstr(uri, "secret=") != NULL);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
