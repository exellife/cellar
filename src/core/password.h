/* cellar — password hashing & token generation (libsodium / argon2id). */
#ifndef CEL_PASSWORD_H
#define CEL_PASSWORD_H

#include <stddef.h>
#include <stdbool.h>

/* Initialize libsodium. Call once at startup. Returns 0 on success. */
int cel_crypto_init(void);

/* Hash `password` into `out` (argon2id, libsodium string format).
 * `out` must be >= 128 bytes. Returns 0 on success, -1 on error. */
int cel_password_hash(const char *password, char *out, size_t out_size);

/* Constant-time verify of `password` against a stored `hash`. */
bool cel_password_verify(const char *hash, const char *password);

/* Write a hex-encoded random token of `nbytes` entropy into `out`
 * (`out` must be >= nbytes*2 + 1). Returns 0 on success, -1 on error. */
int cel_random_token_hex(char *out, size_t out_size, size_t nbytes);

/* Write a numeric one-time code of `digits` digits (4..9, leading zeros kept)
 * into `out` (>= digits+1 bytes), drawn from a uniform bias-free CSPRNG.
 * Returns 0 on success, -1 on error. */
int cel_random_code(char *out, size_t out_size, unsigned digits);

/* Hex-encoded SHA-256 of `token` into `out` (>= 65 bytes). Session tokens are
 * stored hashed at rest, so a DB read-leak yields no usable bearer tokens — the
 * client keeps the raw token; the server hashes it and looks up by the hash.
 * Returns 0 on success, -1 on error. */
int cel_token_hash(const char *token, char *out, size_t out_size);

/* Write a random v4 UUID (36 chars + NUL) into `out` (>= 37 bytes). SQLite has
 * no gen_random_uuid(), so primary keys for the auth tables are minted here.
 * Returns 0 on success, -1 on error. */
int cel_uuid_v4(char *out, size_t out_size);

#endif /* CEL_PASSWORD_H */
