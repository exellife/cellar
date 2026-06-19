#include "openapi.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

/* Map a normalized column type to an OpenAPI (JSON Schema) type + optional format. */
static void oa_type(cJSON *prop, cel_coltype_t t) {
    const char *type = "string", *format = NULL;
    switch (t) {
        case CEL_T_INT:         type = "integer"; format = "int32"; break;
        case CEL_T_BIGINT:      type = "integer"; format = "int64"; break;
        case CEL_T_FLOAT:       type = "number";  format = "double"; break;
        case CEL_T_NUMERIC:     type = "number";  break;
        case CEL_T_BOOL:        type = "boolean"; break;
        case CEL_T_UUID:        type = "string";  format = "uuid"; break;
        case CEL_T_TIMESTAMPTZ: type = "string";  format = "date-time"; break;
        case CEL_T_DATE:        type = "string";  format = "date"; break;
        case CEL_T_JSON:        type = "object";  break;
        case CEL_T_TEXT:
        default:                type = "string";  break;
    }
    cJSON_AddStringToObject(prop, "type", type);
    if (format) cJSON_AddStringToObject(prop, "format", format);
}

/* One JSON-Schema object per table: every column as a typed property. PK and
 * defaulted columns are readOnly (server-set); a NOT NULL column with no default
 * and not the PK is required on create. */
static cJSON *table_schema(const cel_table_t *t) {
    cJSON *sc = cJSON_CreateObject();
    cJSON_AddStringToObject(sc, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(sc, "properties");
    cJSON *required = cJSON_CreateArray();
    for (int i = 0; i < t->ncols; i++) {
        const cel_column_t *c = &t->cols[i];
        cJSON *p = cJSON_AddObjectToObject(props, c->name);
        oa_type(p, c->type);
        if (c->nullable) cJSON_AddBoolToObject(p, "nullable", true);
        if (c->is_pk) cJSON_AddBoolToObject(p, "readOnly", true);   /* PK is server-assigned */
        /* required on create: NOT NULL, no default to fall back on, and not the PK. */
        if (!c->nullable && !c->has_default && !c->is_pk)
            cJSON_AddItemToArray(required, cJSON_CreateString(c->name));
    }
    if (cJSON_GetArraySize(required) > 0) cJSON_AddItemToObject(sc, "required", required);
    else cJSON_Delete(required);
    return sc;
}

/* A $ref to a table's component schema. */
static cJSON *ref_to(const char *table) {
    cJSON *r = cJSON_CreateObject();
    char buf[160];
    snprintf(buf, sizeof buf, "#/components/schemas/%s", table);
    cJSON_AddStringToObject(r, "$ref", buf);
    return r;
}

/* { content: { application/json: { schema: <schema> } } } */
static cJSON *json_content(cJSON *schema) {
    cJSON *o = cJSON_CreateObject();
    cJSON *ct = cJSON_AddObjectToObject(o, "content");
    cJSON *aj = cJSON_AddObjectToObject(ct, "application/json");
    cJSON_AddItemToObject(aj, "schema", schema);
    return o;
}

static cJSON *response(const char *desc, cJSON *schema) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "description", desc);
    if (schema) {
        cJSON *ct = cJSON_AddObjectToObject(r, "content");
        cJSON *aj = cJSON_AddObjectToObject(ct, "application/json");
        cJSON_AddItemToObject(aj, "schema", schema);
    }
    return r;
}

/* A simple query parameter: { name, in:query, schema:{type[,format]} }. */
static cJSON *query_param(const char *name, const char *type, const char *desc) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddStringToObject(p, "in", "query");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddBoolToObject(p, "required", false);
    cJSON *sc = cJSON_AddObjectToObject(p, "schema");
    cJSON_AddStringToObject(sc, "type", type);
    return p;
}

/* {row: $ref} and {rows: [$ref], count: int} wrappers the API actually returns. */
static cJSON *row_wrapper(const char *table) {
    cJSON *sc = cJSON_CreateObject();
    cJSON_AddStringToObject(sc, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(sc, "properties");
    cJSON_AddItemToObject(props, "row", ref_to(table));
    return sc;
}
static cJSON *list_wrapper(const char *table) {
    cJSON *sc = cJSON_CreateObject();
    cJSON_AddStringToObject(sc, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(sc, "properties");
    cJSON *rows = cJSON_AddObjectToObject(props, "rows");
    cJSON_AddStringToObject(rows, "type", "array");
    cJSON_AddItemToObject(rows, "items", ref_to(table));
    cJSON *cnt = cJSON_AddObjectToObject(props, "count");
    cJSON_AddStringToObject(cnt, "type", "integer");
    return sc;
}

struct cJSON *cel_openapi_build(const cel_catalog_t *cat, const char *version) {
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "openapi", "3.0.3");

    cJSON *info = cJSON_AddObjectToObject(doc, "info");
    cJSON_AddStringToObject(info, "title", "cellar API");
    cJSON_AddStringToObject(info, "version", version && *version ? version : "0");
    cJSON_AddStringToObject(info, "description",
        "Auto-generated from the live schema catalog. Bearer token from POST /auth/login.");

    /* bearer security scheme, applied globally (public endpoints ignore it). */
    cJSON *comps = cJSON_AddObjectToObject(doc, "components");
    cJSON *schemes = cJSON_AddObjectToObject(comps, "securitySchemes");
    cJSON *bearer = cJSON_AddObjectToObject(schemes, "bearerAuth");
    cJSON_AddStringToObject(bearer, "type", "http");
    cJSON_AddStringToObject(bearer, "scheme", "bearer");
    cJSON *sec = cJSON_AddArrayToObject(doc, "security");
    cJSON *secReq = cJSON_CreateObject();
    cJSON_AddItemToObject(secReq, "bearerAuth", cJSON_CreateArray());
    cJSON_AddItemToArray(sec, secReq);

    cJSON *schemas = cJSON_AddObjectToObject(comps, "schemas");
    cJSON *paths = cJSON_AddObjectToObject(doc, "paths");

    /* --- login, so the spec shows how to obtain a token --- */
    {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "type", "object");
        cJSON *bp = cJSON_AddObjectToObject(body, "properties");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(bp, "email"), "type", "string");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(bp, "password"), "type", "string");
        cJSON *op = cJSON_CreateObject();
        cJSON_AddStringToObject(op, "summary", "Log in, returns a bearer token");
        cJSON_AddItemToObject(op, "security", cJSON_CreateArray());   /* public */
        cJSON_AddItemToObject(op, "requestBody", json_content(body));
        cJSON *resps = cJSON_AddObjectToObject(op, "responses");
        cJSON_AddItemToObject(resps, "200", response("session token + user", NULL));
        cJSON_AddItemToObject(resps, "401", response("invalid credentials", NULL));
        cJSON *path = cJSON_AddObjectToObject(paths, "/auth/login");
        cJSON_AddItemToObject(path, "post", op);
    }

    for (int i = 0; cat && i < cat->ntables; i++) {
        const cel_table_t *t = &cat->tables[i];
        cJSON_AddItemToObject(schemas, t->name, table_schema(t));

        /* /api/<table> : GET list, POST create */
        {
            cJSON *path = cJSON_CreateObject();

            cJSON *get = cJSON_CreateObject();
            cJSON_AddStringToObject(get, "summary", "List rows");
            cJSON *params = cJSON_AddArrayToObject(get, "parameters");
            cJSON_AddItemToArray(params, query_param("select", "string", "comma-separated columns"));
            cJSON_AddItemToArray(params, query_param("order",  "string", "e.g. -created_at,name"));
            cJSON_AddItemToArray(params, query_param("limit",  "integer", "page size"));
            cJSON_AddItemToArray(params, query_param("offset", "integer", "page offset"));
            cJSON_AddItemToArray(params, query_param("cursor", "string", "keyset cursor (opaque)"));
            cJSON_AddItemToArray(params, query_param("embed",  "string", "related tables to embed"));
            cJSON_AddItemToArray(params, query_param("count",  "string", "set to 'exact' for a total"));
            cJSON_AddItemToArray(params, query_param("where",  "string", "url-encoded JSON filter tree"));
            cJSON *getr = cJSON_AddObjectToObject(get, "responses");
            cJSON_AddItemToObject(getr, "200", response("matching rows", list_wrapper(t->name)));
            cJSON_AddItemToObject(path, "get", get);

            cJSON *post = cJSON_CreateObject();
            cJSON_AddStringToObject(post, "summary", "Create a row");
            cJSON_AddItemToObject(post, "requestBody", json_content(ref_to(t->name)));
            cJSON *postr = cJSON_AddObjectToObject(post, "responses");
            cJSON_AddItemToObject(postr, "201", response("created", row_wrapper(t->name)));
            cJSON_AddItemToObject(postr, "400", response("validation error", NULL));
            cJSON_AddItemToObject(path, "post", post);

            char p[128]; snprintf(p, sizeof p, "/api/%s", t->name);
            cJSON_AddItemToObject(paths, p, path);
        }

        /* /api/<table>/{id} : GET, PATCH, DELETE */
        {
            cJSON *path = cJSON_CreateObject();
            cJSON *idparam = cJSON_CreateObject();
            cJSON_AddStringToObject(idparam, "name", "id");
            cJSON_AddStringToObject(idparam, "in", "path");
            cJSON_AddBoolToObject(idparam, "required", true);
            cJSON_AddStringToObject(cJSON_AddObjectToObject(idparam, "schema"), "type", "string");
            cJSON *pathParams = cJSON_CreateArray();
            cJSON_AddItemToArray(pathParams, idparam);
            cJSON_AddItemToObject(path, "parameters", pathParams);

            cJSON *get = cJSON_CreateObject();
            cJSON_AddStringToObject(get, "summary", "Get one row by id");
            cJSON *gr = cJSON_AddObjectToObject(get, "responses");
            cJSON_AddItemToObject(gr, "200", response("the row", row_wrapper(t->name)));
            cJSON_AddItemToObject(gr, "404", response("not found", NULL));
            cJSON_AddItemToObject(path, "get", get);

            cJSON *patch = cJSON_CreateObject();
            cJSON_AddStringToObject(patch, "summary", "Update a row (partial)");
            cJSON_AddItemToObject(patch, "requestBody", json_content(ref_to(t->name)));
            cJSON *pr = cJSON_AddObjectToObject(patch, "responses");
            cJSON_AddItemToObject(pr, "200", response("updated", row_wrapper(t->name)));
            cJSON_AddItemToObject(path, "patch", patch);

            cJSON *del = cJSON_CreateObject();
            cJSON_AddStringToObject(del, "summary", "Delete a row");
            cJSON *dr = cJSON_AddObjectToObject(del, "responses");
            cJSON_AddItemToObject(dr, "200", response("deleted", NULL));
            cJSON_AddItemToObject(path, "delete", del);

            char p[128]; snprintf(p, sizeof p, "/api/%s/{id}", t->name);
            cJSON_AddItemToObject(paths, p, path);
        }
    }

    return doc;
}
