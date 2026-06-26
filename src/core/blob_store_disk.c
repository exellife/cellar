/* ============================================================================
 * cellar — BlobStore local-disk adapter (default).
 *
 * Lays each blob out as a regular file under <root>/blobs/<key>, with its
 * content-type in a parallel tree <root>/meta/<key>. Two trees (not a ".ct"
 * sidecar beside the data) so a blob key and a meta path can never collide.
 *
 * Writes are atomic: bytes go to a temp file in the destination directory, are
 * fsync'd, then rename(2)'d into place — a concurrent reader sees the whole old
 * object or the whole new one, never a partial write. Keys are validated against
 * the port's rules before they ever touch a path (traversal guard).
 *
 * Dependency-free (libc only) so it builds and unit-tests in isolation.
 * ============================================================================ */
#include "blob_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char *root;
} disk_ctx;

/* Monotonic counter giving temp files a unique name without rand()/clock. */
static _Atomic unsigned long g_tmp_seq = 0;

/* ---- key validation ------------------------------------------------------ */

int blob_key_valid(const char *key) {
    if (!key || !*key) return BLOB_EINVAL;
    size_t n = strlen(key);
    if (n > 1024) return BLOB_EINVAL;
    if (key[0] == '/' || key[n - 1] == '/') return BLOB_EINVAL;

    size_t seg = 0;          /* length of current path segment */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c == '/') {
            if (seg == 0) return BLOB_EINVAL;            /* empty segment "//" */
            seg = 0;
            continue;
        }
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return BLOB_EINVAL;
        seg++;
        /* reject "." and ".." as whole segments */
        if (c == '.' && (seg == 1) &&
            (i + 1 == n || key[i + 1] == '/')) return BLOB_EINVAL;    /* "."  */
        if (c == '.' && (seg == 2) && key[i - 1] == '.' &&
            (i + 1 == n || key[i + 1] == '/')) return BLOB_EINVAL;    /* ".." */
    }
    return BLOB_OK;
}

/* ---- path helpers -------------------------------------------------------- */

/* Build "<root>/<sub>/<key>" into buf. Returns BLOB_OK or BLOB_EINVAL on overflow. */
static int build_path(const disk_ctx *c, const char *sub, const char *key,
                      char *buf, size_t cap) {
    int w = snprintf(buf, cap, "%s/%s/%s", c->root, sub, key);
    if (w < 0 || (size_t)w >= cap) return BLOB_EINVAL;
    return BLOB_OK;
}

/* mkdir -p for every parent directory of `path` (not the final component). */
static int mkdir_parents(const char *path) {
    char tmp[2048];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return BLOB_EINVAL;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return BLOB_EIO;
        *p = '/';
    }
    return BLOB_OK;
}

/* Write `len` bytes to `path` atomically (temp + fsync + rename). */
static int write_atomic(const char *path, const void *data, size_t len) {
    if (mkdir_parents(path) != BLOB_OK) return BLOB_EIO;

    char tmp[2200];
    unsigned long seq = atomic_fetch_add(&g_tmp_seq, 1);
    int w = snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%lu",
                     path, (long)getpid(), seq);
    if (w < 0 || (size_t)w >= sizeof tmp) return BLOB_EINVAL;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return BLOB_EIO;

    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(fd, p + off, len - off);
        if (k < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return BLOB_EIO;
        }
        off += (size_t)k;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return BLOB_EIO; }
    if (close(fd) != 0) { unlink(tmp); return BLOB_EIO; }

    if (rename(tmp, path) != 0) { unlink(tmp); return BLOB_EIO; }
    return BLOB_OK;
}

/* Read the whole file at `path` into a fresh buffer. */
static int read_all(const char *path, void **out, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return (errno == ENOENT) ? BLOB_ENOTFOUND : BLOB_EIO;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return BLOB_EIO; }
    size_t len = (size_t)st.st_size;

    char *buf = malloc(len ? len : 1);
    if (!buf) { close(fd); return BLOB_ENOMEM; }

    size_t off = 0;
    while (off < len) {
        ssize_t k = read(fd, buf + off, len - off);
        if (k < 0) { if (errno == EINTR) continue; free(buf); close(fd); return BLOB_EIO; }
        if (k == 0) break;                 /* truncated under us */
        off += (size_t)k;
    }
    close(fd);
    *out = buf;
    *out_len = off;
    return BLOB_OK;
}

/* ---- vtable -------------------------------------------------------------- */

static int disk_put(void *vctx, const char *key,
                    const void *data, size_t len, const char *content_type) {
    disk_ctx *c = vctx;
    if (blob_key_valid(key) != BLOB_OK) return BLOB_EINVAL;
    if (len && !data) return BLOB_EINVAL;

    char dpath[2048];
    if (build_path(c, "blobs", key, dpath, sizeof dpath) != BLOB_OK) return BLOB_EINVAL;

    int rc = write_atomic(dpath, data, len);
    if (rc != BLOB_OK) return rc;

    /* content-type sidecar in the parallel tree (best-effort but checked) */
    char mpath[2048];
    if (build_path(c, "meta", key, mpath, sizeof mpath) != BLOB_OK) return BLOB_EINVAL;
    const char *ct = content_type ? content_type : "";
    size_t ctn = strlen(ct);
    if (ctn >= sizeof ((blob_obj_t *)0)->content_type) ctn = sizeof ((blob_obj_t *)0)->content_type - 1;
    return write_atomic(mpath, ct, ctn);
}

static int disk_get(void *vctx, const char *key, blob_obj_t *out) {
    disk_ctx *c = vctx;
    if (!out) return BLOB_EINVAL;
    memset(out, 0, sizeof *out);
    if (blob_key_valid(key) != BLOB_OK) return BLOB_EINVAL;

    char dpath[2048];
    if (build_path(c, "blobs", key, dpath, sizeof dpath) != BLOB_OK) return BLOB_EINVAL;

    void *data = NULL; size_t len = 0;
    int rc = read_all(dpath, &data, &len);
    if (rc != BLOB_OK) return rc;
    out->data = data;
    out->len = len;

    /* content-type sidecar (absent => empty) */
    char mpath[2048];
    if (build_path(c, "meta", key, mpath, sizeof mpath) == BLOB_OK) {
        void *m = NULL; size_t mlen = 0;
        if (read_all(mpath, &m, &mlen) == BLOB_OK) {
            if (mlen >= sizeof out->content_type) mlen = sizeof out->content_type - 1;
            memcpy(out->content_type, m, mlen);
            out->content_type[mlen] = '\0';
            free(m);
        }
    }
    return BLOB_OK;
}

static int disk_exists(void *vctx, const char *key) {
    disk_ctx *c = vctx;
    if (blob_key_valid(key) != BLOB_OK) return BLOB_EINVAL;
    char dpath[2048];
    if (build_path(c, "blobs", key, dpath, sizeof dpath) != BLOB_OK) return BLOB_EINVAL;
    struct stat st;
    if (stat(dpath, &st) == 0) return 1;
    if (errno == ENOENT) return 0;
    return BLOB_EIO;
}

static int disk_del(void *vctx, const char *key) {
    disk_ctx *c = vctx;
    if (blob_key_valid(key) != BLOB_OK) return BLOB_EINVAL;

    char dpath[2048], mpath[2048];
    if (build_path(c, "blobs", key, dpath, sizeof dpath) != BLOB_OK) return BLOB_EINVAL;
    if (build_path(c, "meta",  key, mpath, sizeof mpath) != BLOB_OK) return BLOB_EINVAL;

    if (unlink(dpath) != 0 && errno != ENOENT) return BLOB_EIO;
    if (unlink(mpath) != 0 && errno != ENOENT) return BLOB_EIO;
    return BLOB_OK;
}

static char *disk_url(void *vctx, const char *key) {
    (void)vctx; (void)key;
    return NULL;   /* disk blobs are served through an app handler, not a URL */
}

static void disk_destroy(void *vctx) {
    disk_ctx *c = vctx;
    if (!c) return;
    free(c->root);
    free(c);
}

/* ---- constructor --------------------------------------------------------- */

int blob_store_disk_open(const char *root, blob_store_t *out) {
    if (!root || !*root || !out) return BLOB_EINVAL;

    disk_ctx *c = calloc(1, sizeof *c);
    if (!c) return BLOB_ENOMEM;
    c->root = strdup(root);
    if (!c->root) { free(c); return BLOB_ENOMEM; }

    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        free(c->root); free(c);
        return BLOB_EIO;
    }

    out->ctx     = c;
    out->put     = disk_put;
    out->get     = disk_get;
    out->exists  = disk_exists;
    out->del     = disk_del;
    out->url     = disk_url;
    out->destroy = disk_destroy;
    return BLOB_OK;
}

/* ---- shared (port-level) helper ------------------------------------------ */

void blob_obj_free(blob_obj_t *o) {
    if (!o) return;
    free(o->data);
    o->data = NULL;
    o->len = 0;
    o->content_type[0] = '\0';
}
