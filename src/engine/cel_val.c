/* cellar — the hook value ABI (design §8). See cel_val.h. Thin, total-function
 * facade over cJSON so the Lua/FFI side never depends on cJSON's layout. */
#include "cel_val.h"

#include <cjson/cJSON.h>

/* cel_val_t is an opaque alias for a cJSON node. */
static inline cJSON       *J (cel_val_t *v)       { return (cJSON *)v; }
static inline const cJSON *CJ(const cel_val_t *v) { return (const cJSON *)v; }

int cel_val_type(const cel_val_t *v) {
    const cJSON *j = CJ(v);
    if (!j)                return CEL_V_NULL;
    if (cJSON_IsObject(j)) return CEL_V_OBJ;
    if (cJSON_IsArray(j))  return CEL_V_ARR;
    if (cJSON_IsString(j)) return CEL_V_STR;
    if (cJSON_IsNumber(j)) return CEL_V_NUM;
    if (cJSON_IsBool(j))   return CEL_V_BOOL;
    return CEL_V_NULL;   /* null / invalid / raw */
}

const cel_val_t *cel_val_get(const cel_val_t *v, const char *key) {
    const cJSON *j = CJ(v);
    if (!cJSON_IsObject(j) || !key) return NULL;
    return (const cel_val_t *)cJSON_GetObjectItemCaseSensitive(j, key);
}

const cel_val_t *cel_val_at(const cel_val_t *v, int i) {
    const cJSON *j = CJ(v);
    if (!cJSON_IsArray(j) || i < 0) return NULL;
    return (const cel_val_t *)cJSON_GetArrayItem(j, i);
}

int cel_val_len(const cel_val_t *v) {
    const cJSON *j = CJ(v);
    if (cJSON_IsArray(j) || cJSON_IsObject(j)) return cJSON_GetArraySize(j);
    return 0;
}

const char *cel_val_key(const cel_val_t *v, int i) {
    const cJSON *j = CJ(v);
    if (!cJSON_IsObject(j) || i < 0) return NULL;
    const cJSON *c = j->child;
    for (; c && i > 0; c = c->next, i--) { }
    return c ? c->string : NULL;
}

const char *cel_val_str(const cel_val_t *v) {
    const cJSON *j = CJ(v);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

double cel_val_num(const cel_val_t *v) {
    const cJSON *j = CJ(v);
    return cJSON_IsNumber(j) ? j->valuedouble : 0.0;
}

int cel_val_bool(const cel_val_t *v) {
    return cJSON_IsTrue(CJ(v)) ? 1 : 0;
}

/* Replace-or-add `item` under `key` on an object (consumes item; frees it if the
 * target isn't a usable object). */
static void obj_set(cel_val_t *v, const char *key, cJSON *item) {
    cJSON *o = J(v);
    if (!item) return;
    if (!cJSON_IsObject(o) || !key) { cJSON_Delete(item); return; }
    if (cJSON_GetObjectItemCaseSensitive(o, key))
        cJSON_ReplaceItemInObjectCaseSensitive(o, key, item);
    else
        cJSON_AddItemToObject(o, key, item);
}

void cel_val_set_str (cel_val_t *v, const char *key, const char *s) { obj_set(v, key, cJSON_CreateString(s ? s : "")); }
void cel_val_set_num (cel_val_t *v, const char *key, double n)      { obj_set(v, key, cJSON_CreateNumber(n)); }
void cel_val_set_bool(cel_val_t *v, const char *key, int b)         { obj_set(v, key, cJSON_CreateBool(b ? 1 : 0)); }
void cel_val_set_null(cel_val_t *v, const char *key)                { obj_set(v, key, cJSON_CreateNull()); }

void cel_val_unset(cel_val_t *v, const char *key) {
    cJSON *o = J(v);
    if (cJSON_IsObject(o) && key) cJSON_DeleteItemFromObjectCaseSensitive(o, key);
}
