#include "result_json.h"

#include <stdint.h>
#include <stdlib.h>

/* Lossless hex for the rare BLOB cell (JSON has no byte type). */
static cJSON *blob_to_hex_json(const void *bytes, int n) {
    static const char hexd[] = "0123456789abcdef";
    const unsigned char *b = bytes;
    char stackbuf[256];
    char *buf = (n >= 0 && (size_t)n * 2 + 1 <= sizeof stackbuf)
                    ? stackbuf : malloc((size_t)n * 2 + 1);
    if (!buf) return cJSON_CreateString("");
    for (int i = 0; i < n; i++) {
        buf[i * 2]     = hexd[b[i] >> 4];
        buf[i * 2 + 1] = hexd[b[i] & 0xF];
    }
    buf[n * 2] = '\0';
    cJSON *j = cJSON_CreateString(buf);
    if (buf != stackbuf) free(buf);
    return j;
}

/* Type the cell from the value's runtime storage class (no catalog hint). */
static cJSON *cell_runtime(sqlite3_stmt *st, int c) {
    switch (sqlite3_column_type(st, c)) {
        case SQLITE_NULL:    return cJSON_CreateNull();
        case SQLITE_INTEGER: return cJSON_CreateNumber((double)sqlite3_column_int64(st, c));
        case SQLITE_FLOAT:   return cJSON_CreateNumber(sqlite3_column_double(st, c));
        case SQLITE_BLOB:    return blob_to_hex_json(sqlite3_column_blob(st, c),
                                                     sqlite3_column_bytes(st, c));
        case SQLITE_TEXT:
        default: {
            const unsigned char *t = sqlite3_column_text(st, c);
            return cJSON_CreateString(t ? (const char *)t : "");
        }
    }
}

/* Type the cell from the declared catalog type. NULL short-circuits regardless
 * of declared type. INT/BIGINT are emitted as JSON numbers via double — very
 * large int8 can lose exactness (same caveat as the Postgres path). */
static cJSON *cell_typed(sqlite3_stmt *st, int c, cel_coltype_t type) {
    if (sqlite3_column_type(st, c) == SQLITE_NULL)
        return cJSON_CreateNull();
    switch (type) {
        case CEL_T_INT:
        case CEL_T_BIGINT:
            return cJSON_CreateNumber((double)sqlite3_column_int64(st, c));
        case CEL_T_FLOAT:
        case CEL_T_NUMERIC:
            return cJSON_CreateNumber(sqlite3_column_double(st, c));
        case CEL_T_BOOL:
            return cJSON_CreateBool(sqlite3_column_int(st, c) != 0);
        case CEL_T_JSON: {
            const char *txt = (const char *)sqlite3_column_text(st, c);
            cJSON *j = txt ? cJSON_Parse(txt) : NULL;
            return j ? j : cJSON_CreateString(txt ? txt : "");
        }
        default:    /* TEXT / UUID / TIMESTAMPTZ / DATE / OTHER → string */
            return cell_runtime(st, c);   /* covers TEXT, and BLOB defensively */
    }
}

cJSON *cel_stmt_rows_to_json(sqlite3_stmt *st, const cel_table_t *t) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    int ncols = sqlite3_column_count(st);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *obj = cJSON_CreateObject();
        for (int c = 0; c < ncols; c++) {
            const char *name = sqlite3_column_name(st, c);
            const cel_column_t *col = t ? cel_table_column(t, name) : NULL;
            cJSON *val = col ? cell_typed(st, c, col->type) : cell_runtime(st, c);
            cJSON_AddItemToObject(obj, name, val);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

cJSON *cel_stmt_result_to_json(sqlite3_stmt *st) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    int ncols = sqlite3_column_count(st);
    while (sqlite3_step(st) == SQLITE_ROW) {
        cJSON *obj = cJSON_CreateObject();
        for (int c = 0; c < ncols; c++)
            cJSON_AddItemToObject(obj, sqlite3_column_name(st, c), cell_runtime(st, c));
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}
