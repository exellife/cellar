/* cellar — base64url unit test (no DB). */
#include "base64url.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

int main(void) {
    printf("base64url\n");

    /* known vector: "hello" -> "aGVsbG8" (no padding) */
    char enc[64];
    chk("encode ok", cel_b64url_encode((const unsigned char *)"hello", 5, enc, sizeof enc) == 0);
    chk("encode = aGVsbG8", strcmp(enc, "aGVsbG8") == 0);

    /* round-trip arbitrary bytes (incl. ones that map to '-' and '_') */
    const unsigned char raw[] = {0x00, 0xff, 0x3e, 0xfb, 0xef, 0xbe, 0x01, 0x42, 0x99};
    char b[64];
    unsigned char back[64];
    size_t n = 0;
    chk("rt encode", cel_b64url_encode(raw, sizeof raw, b, sizeof b) == 0);
    chk("rt decode", cel_b64url_decode(b, strlen(b), back, sizeof back, &n) == 0);
    chk("rt length", n == sizeof raw);
    chk("rt bytes equal", n == sizeof raw && memcmp(raw, back, n) == 0);

    /* url-safe alphabet, never '+' '/' '=' */
    int clean = 1;
    for (const char *p = b; *p; p++) if (*p == '+' || *p == '/' || *p == '=') clean = 0;
    chk("url-safe alphabet", clean);

    /* invalid characters are rejected */
    chk("rejects invalid char", cel_b64url_decode("!!", 2, back, sizeof back, &n) != 0);

    /* small output buffer is rejected, not overflowed */
    chk("encode rejects tiny out", cel_b64url_encode((const unsigned char *)"hello", 5, enc, 2) != 0);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
