#include "base64url.h"

#include <stdint.h>

static const char ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int dec_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int pgf_b64url_encode(const unsigned char *in, size_t inlen, char *out, size_t out_size) {
    size_t need = (inlen * 8 + 5) / 6;        /* output chars, no padding */
    if (out_size < need + 1) return -1;
    size_t oi = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 6) { out[oi++] = ENC[(buf >> (bits - 6)) & 63]; bits -= 6; }
    }
    if (bits > 0) out[oi++] = ENC[(buf << (6 - bits)) & 63];
    out[oi] = '\0';
    return 0;
}

int pgf_b64url_decode(const char *in, size_t inlen, unsigned char *out,
                      size_t out_size, size_t *outlen) {
    uint32_t buf = 0;
    int bits = 0;
    size_t oi = 0;
    for (size_t i = 0; i < inlen; i++) {
        int v = dec_val((unsigned char)in[i]);
        if (v < 0) return -1;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            if (oi >= out_size) return -1;
            out[oi++] = (buf >> (bits - 8)) & 0xFF;
            bits -= 8;
        }
    }
    *outlen = oi;
    return 0;
}
