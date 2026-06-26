/* ============================================================================
 * cellar — BlobStore: a content-addressable object store, behind a port.
 *
 * A generic engine primitive: store/fetch/delete opaque byte blobs by key, with
 * an associated content-type. The engine depends ONLY on this interface (a vtable
 * + opaque ctx); it never knows whether bytes live on local disk, S3/R2, or a CDN.
 * This is the port-first discipline (the lesson from the SQLite coupling): no impl
 * assumptions — no filesystem paths, no bucket names — leak through this header.
 *
 * Keys are opaque, caller-chosen strings (e.g. "ab/cd/abcd1234.jpg"). They are a
 * logical namespace, NOT filesystem paths: an adapter must map them safely and
 * reject traversal. Valid key bytes: [A-Za-z0-9._-] and '/' as a separator; no
 * empty segment, no "." / ".." segment, no leading/trailing '/'.
 *
 * Default adapter: local disk (blob_store_disk.c). Later adapters (S3/R2 + CDN)
 * implement the same vtable; the engine is unchanged.
 * ============================================================================ */
#ifndef CEL_BLOB_STORE_H
#define CEL_BLOB_STORE_H

#include <stddef.h>

/* Return codes. 0 == success; negative == failure. */
enum {
    BLOB_OK        =  0,
    BLOB_EINVAL    = -1,   /* bad argument or malformed/unsafe key            */
    BLOB_ENOTFOUND = -2,   /* no object stored under this key                 */
    BLOB_EIO       = -3,   /* underlying I/O / transport error                */
    BLOB_ENOMEM    = -4,   /* allocation failed                               */
    BLOB_ETOOBIG   = -5    /* object exceeds an adapter/caller-imposed cap    */
};

/* A fetched object. `data` is heap-allocated by get(); the caller owns it and
 * frees it with blob_obj_free(). `content_type` is the value supplied at put()
 * time (empty string if none was given). */
typedef struct {
    void  *data;
    size_t len;
    char   content_type[128];
} blob_obj_t;

/* Free the buffer owned by a blob_obj_t and zero it. Safe on a zeroed/empty obj. */
void blob_obj_free(blob_obj_t *o);

/* The port: a vtable + opaque ctx. Construct via an adapter (e.g.
 * blob_store_disk_open); use through these function pointers only. */
typedef struct blob_store {
    void *ctx;

    /* Store `len` bytes under `key`, replacing any existing object atomically
     * (a reader sees either the old object or the new one, never a torn write).
     * `content_type` may be NULL/empty. Returns BLOB_OK or a negative code. */
    int (*put)(void *ctx, const char *key,
               const void *data, size_t len, const char *content_type);

    /* Fetch the object under `key` into `*out` (allocates out->data). Returns
     * BLOB_OK, BLOB_ENOTFOUND, or another negative code. On failure `*out` is
     * left zeroed. */
    int (*get)(void *ctx, const char *key, blob_obj_t *out);

    /* True (1) if an object exists under `key`, 0 if not, negative on error. */
    int (*exists)(void *ctx, const char *key);

    /* Delete the object under `key`. Deleting a missing key is BLOB_OK
     * (idempotent). Returns BLOB_OK or a negative code. */
    int (*del)(void *ctx, const char *key);

    /* A directly-fetchable URL for `key`, or NULL if this adapter serves blobs
     * only through an application handler (the disk adapter returns NULL). The
     * returned string is heap-allocated; the caller frees it. */
    char *(*url)(void *ctx, const char *key);

    /* Release all adapter resources. The handle is unusable afterward. */
    void (*destroy)(void *ctx);
} blob_store_t;

/* Validate a key against the rules documented above. Returns BLOB_OK or
 * BLOB_EINVAL. Exposed so callers can reject bad keys before constructing one. */
int blob_key_valid(const char *key);

/* ---- default adapter: local disk -----------------------------------------
 * Stores each blob as a file under `root` (created if absent), with its
 * content-type in a sidecar. `root` is owned by the caller and must outlive the
 * store. Returns BLOB_OK and fills `*out` (a vtable bound to a private ctx), or
 * a negative code. Free with out->destroy(out->ctx). */
int blob_store_disk_open(const char *root, blob_store_t *out);

#endif /* CEL_BLOB_STORE_H */
