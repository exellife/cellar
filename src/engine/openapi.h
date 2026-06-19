/* cellar — generate an OpenAPI 3.0 document for the data API from the live
 * schema catalog. Served at GET /openapi.json so clients/tools can introspect and
 * generate SDKs without a hand-written spec. */
#ifndef CEL_OPENAPI_H
#define CEL_OPENAPI_H

#include "schema_catalog.h"

struct cJSON;

/* Build an OpenAPI 3.0 document describing the table CRUD API (+ login) for `cat`.
 * `version` is the build/API version string for info.version. Caller owns the
 * result (cJSON_Delete). */
struct cJSON *cel_openapi_build(const cel_catalog_t *cat, const char *version);

#endif /* CEL_OPENAPI_H */
