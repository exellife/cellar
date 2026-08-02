/* ============================================================================
 * cellar — TOTP (RFC 6238) time-based one-time passwords for 2FA.
 *
 * Standard 6-digit / 30-second / HMAC-SHA1 codes, compatible with Google
 * Authenticator, Authy, 1Password, etc. HMAC-SHA1 comes from OpenSSL (already
 * linked via portico's TLS / libpq) and the random secret from libsodium — no
 * new dependency. This module is pure crypto: no DB, no policy. The DB-backed
 * enrollment/verification flow lives in mfa.{c,h}.
 * ============================================================================ */
#ifndef CEL_TOTP_H
#define CEL_TOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Generate a fresh random base32 secret (160-bit) into `out` (NUL-terminated;
 * `out` should be >= 33 bytes). Returns 0 on success. */
int cel_totp_generate_secret(char *out, size_t out_size);

/* Compute the 6-digit code for `secret_b32` at unix time `t` into `out`
 * (>= 7 bytes). Exposed for testing against the RFC 6238 vectors; verification
 * uses it internally with now ± window. Returns 0 on success. */
int cel_totp_code_at(const char *secret_b32, uint64_t unix_time,
                     char *out, size_t out_size);

/* True if 6-digit `code` matches `secret_b32` within ±`window` 30s steps of now
 * (window 1 tolerates a little clock drift). Spaces in `code` are ignored; the
 * comparison is constant-time. */
bool cel_totp_verify(const char *secret_b32, const char *code, int window);

/* Like cel_totp_verify, but returns the matched time-step (unix_time / 30) so the
 * caller can enforce single-use by rejecting a step it already accepted (RFC 6238
 * §5.2 replay prevention). Returns the matched step (>= 0), or -1 if no window
 * matched / the code is malformed. Same constant-work sweep as cel_totp_verify. */
int64_t cel_totp_verify_step(const char *secret_b32, const char *code, int window);

/* Build an `otpauth://totp/...` URI (the string an enrollment QR encodes) into
 * `out`. `issuer` and `account` should be URL-safe. Returns 0 on success. */
int cel_totp_uri(const char *secret_b32, const char *issuer, const char *account,
                 char *out, size_t out_size);

#endif /* CEL_TOTP_H */
