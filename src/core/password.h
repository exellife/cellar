/* pgforge — password hashing & token generation (libsodium / argon2id). */
#ifndef PGF_PASSWORD_H
#define PGF_PASSWORD_H

#include <stddef.h>
#include <stdbool.h>

/* Initialize libsodium. Call once at startup. Returns 0 on success. */
int pgf_crypto_init(void);

/* Hash `password` into `out` (argon2id, libsodium string format).
 * `out` must be >= 128 bytes. Returns 0 on success, -1 on error. */
int pgf_password_hash(const char *password, char *out, size_t out_size);

/* Constant-time verify of `password` against a stored `hash`. */
bool pgf_password_verify(const char *hash, const char *password);

/* Write a hex-encoded random token of `nbytes` entropy into `out`
 * (`out` must be >= nbytes*2 + 1). Returns 0 on success, -1 on error. */
int pgf_random_token_hex(char *out, size_t out_size, size_t nbytes);

/* Hex-encoded SHA-256 of `token` into `out` (>= 65 bytes). Session tokens are
 * stored hashed at rest, so a DB read-leak yields no usable bearer tokens — the
 * client keeps the raw token; the server hashes it and looks up by the hash.
 * Returns 0 on success, -1 on error. */
int pgf_token_hash(const char *token, char *out, size_t out_size);

#endif /* PGF_PASSWORD_H */
