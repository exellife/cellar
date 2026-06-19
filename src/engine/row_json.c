#include "row_json.h"

#include <stdlib.h>

/* Convert one text cell to a typed JSON value. NUMERIC/BIGINT are emitted as
 * JSON numbers (note: very large int8/high-precision numeric may lose exactness
 * through double — acceptable for dashboard display; revisit if needed). */
static cJSON *cell_to_json(const char *txt, pgf_coltype_t type) {
    switch (type) {
        case PGF_T_INT:
        case PGF_T_BIGINT:
            return cJSON_CreateNumber((double)strtoll(txt, NULL, 10));
        case PGF_T_FLOAT:
        case PGF_T_NUMERIC:
            return cJSON_CreateNumber(strtod(txt, NULL));
        case PGF_T_BOOL:
            return cJSON_CreateBool(txt[0] == 't');
        case PGF_T_JSON: {
            cJSON *j = cJSON_Parse(txt);
            return j ? j : cJSON_CreateString(txt);
        }
        default:
            return cJSON_CreateString(txt);
    }
}

/* Map a Postgres type OID to our normalized type (for table-less results). The
 * OIDs are stable built-ins; anything unrecognized serializes as a string. */
static pgf_coltype_t oid_to_type(Oid oid) {
    switch (oid) {
        case 16:                   return PGF_T_BOOL;     /* bool */
        case 20: case 21: case 23: return PGF_T_BIGINT;   /* int8 / int2 / int4 */
        case 700: case 701:        return PGF_T_FLOAT;    /* float4 / float8 */
        case 1700:                 return PGF_T_NUMERIC;  /* numeric */
        case 114: case 3802:       return PGF_T_JSON;     /* json / jsonb */
        default:                   return PGF_T_TEXT;
    }
}

cJSON *pgf_result_to_json(PGresult *res) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    int nrows = PQntuples(res), nfields = PQnfields(res);
    for (int r = 0; r < nrows; r++) {
        cJSON *obj = cJSON_CreateObject();
        for (int f = 0; f < nfields; f++) {
            const char *name = PQfname(res, f);
            if (PQgetisnull(res, r, f)) {
                cJSON_AddItemToObject(obj, name, cJSON_CreateNull());
                continue;
            }
            cJSON_AddItemToObject(obj, name,
                cell_to_json(PQgetvalue(res, r, f), oid_to_type(PQftype(res, f))));
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

cJSON *pgf_rows_to_json(PGresult *res, const pgf_table_t *t) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    int nrows = PQntuples(res);
    int nfields = PQnfields(res);

    /* Resolve each result field's normalized type once, by name. */
    for (int r = 0; r < nrows; r++) {
        cJSON *obj = cJSON_CreateObject();
        for (int f = 0; f < nfields; f++) {
            const char *name = PQfname(res, f);
            if (PQgetisnull(res, r, f)) {
                cJSON_AddItemToObject(obj, name, cJSON_CreateNull());
                continue;
            }
            const char *val = PQgetvalue(res, r, f);
            const pgf_column_t *col = pgf_table_column(t, name);
            pgf_coltype_t type = col ? col->type : PGF_T_TEXT;
            cJSON_AddItemToObject(obj, name, cell_to_json(val, type));
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}
