/* pgforge — base64url (RFC 4648 §5, no padding). Used for opaque cursor tokens. */
#ifndef PGF_BASE64URL_H
#define PGF_BASE64URL_H

#include <stddef.h>

/* Encode `inlen` bytes into `out` (NUL-terminated, no '=' padding). `out` must be
 * >= 4*ceil(inlen/3)+1. Returns 0 on success, -1 if `out` is too small. */
int pgf_b64url_encode(const unsigned char *in, size_t inlen, char *out, size_t out_size);

/* Decode `inlen` base64url chars into `out`, writing the length to *outlen.
 * Returns 0 on success, -1 on an invalid char or insufficient space. */
int pgf_b64url_decode(const char *in, size_t inlen, unsigned char *out,
                      size_t out_size, size_t *outlen);

#endif /* PGF_BASE64URL_H */
