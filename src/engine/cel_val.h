/* ============================================================================
 * cellar — the hook value ABI (design §8).
 *
 * An opaque JSON value handle (object / array / scalar) passed to Lua hooks as
 * `who` / `input` / `row` / `args` / `change`. It wraps cJSON internally, but the
 * hook ABI deliberately never exposes cJSON's struct — these are the exact, stable
 * functions cel_hooks' `ffi.cdef` declares, so the Lua side calls them directly via
 * FFI with no binding glue (the Lua prelude wraps them in table-like sugar).
 *
 * Readers are total: a wrong-type or absent access returns a benign default
 * (NULL / 0), never faults — a hook can't crash the worker by reading oddly.
 * Mutators act on an OBJECT value (e.g. before()'s writable `input`); they are
 * no-ops on a non-object or with a NULL handle.
 * ============================================================================ */
#ifndef CEL_VAL_H
#define CEL_VAL_H

/* Opaque to every consumer (and to the FFI cdef); cel_val.c casts to cJSON. */
typedef struct cel_val cel_val_t;

/* Value kind, returned by cel_val_type. */
enum { CEL_V_NULL = 0, CEL_V_BOOL, CEL_V_NUM, CEL_V_STR, CEL_V_OBJ, CEL_V_ARR };

/* ---- reads (total; defaults on absence / type mismatch) ---- */
int              cel_val_type(const cel_val_t *v);                  /* CEL_V_*           */
const cel_val_t *cel_val_get (const cel_val_t *v, const char *key); /* object field, NULL if none */
const cel_val_t *cel_val_at  (const cel_val_t *v, int i);           /* array element, NULL if oob */
int              cel_val_len (const cel_val_t *v);                  /* array/object count, else 0 */
const char      *cel_val_key (const cel_val_t *v, int i);          /* i-th object key name, NULL if oob */
const char      *cel_val_str (const cel_val_t *v);                 /* string value, NULL if not a string */
double           cel_val_num (const cel_val_t *v);                 /* number value, 0 if not a number */
int              cel_val_bool(const cel_val_t *v);                 /* 0/1; 0 if not a bool */

/* ---- mutations (object only; e.g. before()'s `input`) ---- */
void cel_val_set_str (cel_val_t *v, const char *key, const char *s);
void cel_val_set_num (cel_val_t *v, const char *key, double n);
void cel_val_set_bool(cel_val_t *v, const char *key, int b);
void cel_val_set_null(cel_val_t *v, const char *key);
void cel_val_unset   (cel_val_t *v, const char *key);

/* ---- owned-value construction ----
 * For building bind-param arrays (cellar.query/exec) and freeing query results
 * that cross back to Lua. cel_val_free frees an OWNED value (no-op on NULL); never
 * call it on a borrowed handle returned by cel_val_get / cel_val_at. */
cel_val_t *cel_val_new_array(void);
void       cel_val_push_str (cel_val_t *arr, const char *s);
void       cel_val_push_num (cel_val_t *arr, double n);
void       cel_val_push_bool(cel_val_t *arr, int b);
void       cel_val_push_null(cel_val_t *arr);
void       cel_val_free     (cel_val_t *v);

#endif /* CEL_VAL_H */
