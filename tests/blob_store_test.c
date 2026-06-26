/* cellar — BlobStore disk-adapter unit test (no DB). */
#include "blob_store.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

/* ---- key validation (traversal guard) ------------------------------------ */
static void test_keys(void) {
    printf("key validation\n");
    chk("accepts simple",      blob_key_valid("abcd1234.jpg") == BLOB_OK);
    chk("accepts fan-out",     blob_key_valid("ab/cd/abcd1234.jpg") == BLOB_OK);
    chk("accepts dot-mid",     blob_key_valid("a.b.c") == BLOB_OK);
    chk("rejects NULL",        blob_key_valid(NULL) == BLOB_EINVAL);
    chk("rejects empty",       blob_key_valid("") == BLOB_EINVAL);
    chk("rejects leading /",   blob_key_valid("/etc/passwd") == BLOB_EINVAL);
    chk("rejects trailing /",  blob_key_valid("a/") == BLOB_EINVAL);
    chk("rejects ..",          blob_key_valid("a/../b") == BLOB_EINVAL);
    chk("rejects .. tail",     blob_key_valid("a/..") == BLOB_EINVAL);
    chk("rejects .",           blob_key_valid("a/./b") == BLOB_EINVAL);
    chk("rejects . tail",      blob_key_valid("a/.") == BLOB_EINVAL);
    chk("rejects //",          blob_key_valid("a//b") == BLOB_EINVAL);
    chk("rejects backslash",   blob_key_valid("a\\b") == BLOB_EINVAL);
    chk("rejects space",       blob_key_valid("a b") == BLOB_EINVAL);
    chk("rejects null byte ok as strlen", blob_key_valid("a") == BLOB_OK);
}

/* ---- basic lifecycle ----------------------------------------------------- */
static void test_lifecycle(blob_store_t *s) {
    printf("lifecycle\n");
    blob_obj_t o;

    /* missing key */
    chk("get missing => ENOTFOUND", s->get(s->ctx, "no/such/key", &o) == BLOB_ENOTFOUND);
    chk("missing leaves obj empty", o.data == NULL && o.len == 0);
    chk("exists missing => 0",      s->exists(s->ctx, "no/such/key") == 0);

    /* put + roundtrip with content-type */
    const char *body = "hello blobstore";
    chk("put ok", s->put(s->ctx, "ab/cd/greeting.txt", body, strlen(body), "text/plain") == BLOB_OK);
    chk("exists => 1", s->exists(s->ctx, "ab/cd/greeting.txt") == 1);
    chk("get ok", s->get(s->ctx, "ab/cd/greeting.txt", &o) == BLOB_OK);
    chk("len matches", o.len == strlen(body));
    chk("bytes match", o.len == strlen(body) && memcmp(o.data, body, o.len) == 0);
    chk("content-type preserved", strcmp(o.content_type, "text/plain") == 0);
    blob_obj_free(&o);

    /* overwrite (atomic replace) */
    const char *body2 = "replaced";
    chk("overwrite ok", s->put(s->ctx, "ab/cd/greeting.txt", body2, strlen(body2), "text/markdown") == BLOB_OK);
    chk("get after overwrite", s->get(s->ctx, "ab/cd/greeting.txt", &o) == BLOB_OK);
    chk("overwrite bytes", o.len == strlen(body2) && memcmp(o.data, body2, o.len) == 0);
    chk("overwrite content-type", strcmp(o.content_type, "text/markdown") == 0);
    blob_obj_free(&o);

    /* empty content-type */
    chk("put no ctype", s->put(s->ctx, "k/empty-ct", "x", 1, NULL) == BLOB_OK);
    chk("get no ctype", s->get(s->ctx, "k/empty-ct", &o) == BLOB_OK);
    chk("empty content-type", o.content_type[0] == '\0');
    blob_obj_free(&o);

    /* delete (then idempotent re-delete) */
    chk("del ok", s->del(s->ctx, "ab/cd/greeting.txt") == BLOB_OK);
    chk("gone after del", s->exists(s->ctx, "ab/cd/greeting.txt") == 0);
    chk("del idempotent", s->del(s->ctx, "ab/cd/greeting.txt") == BLOB_OK);

    /* bad key rejected at the vtable too */
    chk("put bad key => EINVAL", s->put(s->ctx, "../escape", "x", 1, NULL) == BLOB_EINVAL);
    chk("get bad key => EINVAL", s->get(s->ctx, "../escape", &o) == BLOB_EINVAL);

    /* url: disk adapter serves via handler */
    chk("url => NULL (disk)", s->url(s->ctx, "ab/cd/greeting.txt") == NULL);
}

/* ---- large blob ---------------------------------------------------------- */
static void test_large(blob_store_t *s) {
    printf("large blob\n");
    size_t n = 5u * 1024 * 1024;       /* 5 MiB */
    unsigned char *buf = malloc(n);
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)((i * 1103515245u + 12345u) >> 7);

    chk("put 5MiB", s->put(s->ctx, "big/blob.bin", buf, n, "application/octet-stream") == BLOB_OK);
    blob_obj_t o;
    chk("get 5MiB", s->get(s->ctx, "big/blob.bin", &o) == BLOB_OK);
    chk("5MiB len", o.len == n);
    chk("5MiB bytes", o.len == n && memcmp(o.data, buf, n) == 0);
    blob_obj_free(&o);
    free(buf);
}

/* ---- concurrency --------------------------------------------------------- */
#define NTHREAD 8
#define NOPS    200
typedef struct { blob_store_t *s; int id; int ok; } worker_arg;

static void *worker(void *vp) {
    worker_arg *a = vp;
    a->ok = 1;
    char key[64];
    for (int i = 0; i < NOPS; i++) {
        snprintf(key, sizeof key, "cc/t%d/obj%d", a->id, i);
        char val[64];
        int vn = snprintf(val, sizeof val, "thread-%d-op-%d", a->id, i);
        if (a->s->put(a->s->ctx, key, val, (size_t)vn, "text/plain") != BLOB_OK) { a->ok = 0; continue; }
        blob_obj_t o;
        if (a->s->get(a->s->ctx, key, &o) != BLOB_OK) { a->ok = 0; continue; }
        if (o.len != (size_t)vn || memcmp(o.data, val, o.len) != 0) a->ok = 0;
        blob_obj_free(&o);
    }
    return NULL;
}

static void test_concurrent(blob_store_t *s) {
    printf("concurrency\n");
    pthread_t th[NTHREAD];
    worker_arg args[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        args[i] = (worker_arg){ .s = s, .id = i, .ok = 0 };
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    int all = 1;
    for (int i = 0; i < NTHREAD; i++) { pthread_join(th[i], NULL); if (!args[i].ok) all = 0; }
    chk("all threads consistent", all);

    /* spot-check a few survived */
    blob_obj_t o;
    chk("post-cc readback", s->get(s->ctx, "cc/t3/obj42", &o) == BLOB_OK);
    if (o.data) blob_obj_free(&o);
}

int main(void) {
    printf("blob_store (disk)\n");

    test_keys();

    char tmpl[] = "/tmp/cellar_blob_XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) { perror("mkdtemp"); return 2; }

    blob_store_t s;
    if (blob_store_disk_open(root, &s) != BLOB_OK) { fprintf(stderr, "open failed\n"); return 2; }

    test_lifecycle(&s);
    test_large(&s);
    test_concurrent(&s);

    s.destroy(s.ctx);

    /* clean up the temp tree */
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "warn: cleanup failed\n");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
