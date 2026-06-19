/* pgforge — generate an OpenAPI 3.0 document for the data API from the live
 * schema catalog. Served at GET /openapi.json so clients/tools can introspect and
 * generate SDKs without a hand-written spec. */
#ifndef PGF_OPENAPI_H
#define PGF_OPENAPI_H

#include "schema_catalog.h"

struct cJSON;

/* Build an OpenAPI 3.0 document describing the table CRUD API (+ login) for `cat`.
 * `version` is the build/API version string for info.version. Caller owns the
 * result (cJSON_Delete). */
struct cJSON *pgf_openapi_build(const pgf_catalog_t *cat, const char *version);

#endif /* PGF_OPENAPI_H */
