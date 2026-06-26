/* ============================================================================
 * cellar — media upload/serve handlers (E1.1 / E1.2).
 *
 * Thin, generic glue over two engine modules — the image lib (validate/
 * re-encode) and BlobStore (storage) — tied to the request loop. Domain-
 * agnostic: it stores validated image bytes and hands back an id + variant
 * names; it never knows a blob is a "listing photo". A bundle associates the
 * returned id with its own rows (e.g. a listings.photos array).
 *
 * Blobs live in a per-app store rooted at <bundle_dir>/media. The upload
 * re-encodes every image to canonical JPEG variants (untrusted bytes are never
 * stored or served verbatim); serve streams a variant back with a long
 * immutable cache (ids are unique per upload).
 * ============================================================================ */
#ifndef CEL_MEDIA_H
#define CEL_MEDIA_H

#include "portico.h"
#include "engine/policy.h"   /* cel_identity_t */

/* POST /media — body is raw image bytes (Content-Type is a hint; the format is
 * sniffed). Auth required. Validates + re-encodes to JPEG variants, stores them,
 * and replies 201 {id, width, height, variants:[...]}. Returns the HTTP status. */
int cel_media_upload(const portico_request_t *req, portico_response_t *res,
                     const cel_identity_t *who);

/* GET /media/<id>/<variant> — serve a stored variant (or 404). `id`/`variant`
 * are the parsed path segments. Returns the HTTP status set. */
int cel_media_serve(const portico_request_t *req, portico_response_t *res,
                    const char *id, const char *variant);

/* Max accepted upload body in bytes (0 => built-in default). From CEL_MEDIA_MAX.
 * Stays under portico's 16 MiB transport cap. */
void cel_media_set_max(size_t bytes);

/* True if `id` is a well-formed media id (32 lowercase hex chars). Exposed so the
 * router can reject junk paths before calling serve. */
int cel_media_id_valid(const char *id);

#endif /* CEL_MEDIA_H */
