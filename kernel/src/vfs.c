/* See vfs.h. */
#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "fat32.h"
#include "syscall.h"
#include "string.h"
#include "serial.h"

#define VFS_PATH_MAX 192

int vfs_init(void) {
    if (!fat32_init()) {
        serial_print("vfs: no writable filesystem available.\n");
        return 0;
    }
    return 1;
}

/* Joins `cwd` and `path` into a single absolute path in `out` (bounded
 * to VFS_PATH_MAX) -- `path` itself if it's already absolute, otherwise
 * "cwd/path". Doesn't canonicalize "." or ".." components; anything
 * containing them simply won't resolve (fat32_lookup() has no special
 * handling for them either -- see fat32.h's doc comment). Returns 1 on
 * success, 0 if the joined path doesn't fit. */
static int make_absolute(const char *cwd, const char *path, char *out, uint64_t out_cap) {
    if (path[0] == '/') {
        uint64_t len = 0;
        while (path[len]) {
            len++;
        }
        if (len + 1 > out_cap) {
            return 0;
        }
        memcpy(out, path, len + 1);
        return 1;
    }

    uint64_t cwd_len = 0;
    while (cwd[cwd_len]) {
        cwd_len++;
    }
    uint64_t path_len = 0;
    while (path[path_len]) {
        path_len++;
    }
    /* cwd + '/' + path + '\0', collapsing a trailing '/' cwd already has
     * (e.g. root, "/") so paths don't end up with a doubled slash. */
    int cwd_has_trailing_slash = (cwd_len > 0 && cwd[cwd_len - 1] == '/');
    uint64_t sep_len = cwd_has_trailing_slash ? 0 : 1;
    if (cwd_len + sep_len + path_len + 1 > out_cap) {
        return 0;
    }
    memcpy(out, cwd, cwd_len);
    if (!cwd_has_trailing_slash) {
        out[cwd_len] = '/';
    }
    memcpy(out + cwd_len + sep_len, path, path_len + 1);
    return 1;
}

/* Collapses "." and ".." components out of an already-absolute path, in
 * place -- fat32_lookup() has no idea what either one means (see
 * fat32.h's doc comment), so anything that reaches it must already be
 * fully resolved. ".." past root is simply clamped to root (same as
 * every real OS: "/.." == "/"), and a bare "/" always survives as
 * itself. Bounded by VFS_PATH_MAX-sized inputs, so the rebuilt path can
 * never be longer than what came in. */
#define VFS_MAX_COMPONENTS 32
static void normalize_path(char *path) {
    const char *seg_starts[VFS_MAX_COMPONENTS];
    uint64_t seg_lens[VFS_MAX_COMPONENTS];
    int depth = 0;

    const char *p = path;
    while (*p == '/') {
        p++;
    }
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        uint64_t len = (uint64_t)(p - start);
        if (len == 1 && start[0] == '.') {
            /* skip */
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0) {
                depth--;
            }
        } else if (len > 0 && depth < VFS_MAX_COMPONENTS) {
            seg_starts[depth] = start;
            seg_lens[depth] = len;
            depth++;
        }
        while (*p == '/') {
            p++;
        }
    }

    char result[VFS_PATH_MAX];
    uint64_t out_len = 0;
    result[out_len++] = '/';
    for (int i = 0; i < depth; i++) {
        if (i > 0) {
            result[out_len++] = '/';
        }
        memcpy(result + out_len, seg_starts[i], seg_lens[i]);
        out_len += seg_lens[i];
    }
    result[out_len] = '\0';
    memcpy(path, result, out_len + 1);
}

int vfs_open(const char *cwd, const char *path, int flags, struct fat32_file *out) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);

    if (fat32_lookup(abs, out)) {
        if ((flags & O_TRUNC) && !out->is_dir) {
            return fat32_create(abs, out); /* fat32_create() truncates an existing file */
        }
        return 1;
    }
    if (flags & O_CREAT) {
        return fat32_create(abs, out);
    }
    return 0;
}

int vfs_mkdir(const char *cwd, const char *path) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);
    return fat32_mkdir(abs);
}

int vfs_unlink(const char *cwd, const char *path) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);
    return fat32_unlink(abs);
}

int vfs_rmdir(const char *cwd, const char *path) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);
    return fat32_rmdir(abs);
}

int vfs_rename(const char *cwd, const char *old_path, const char *new_path) {
    char abs_old[VFS_PATH_MAX];
    char abs_new[VFS_PATH_MAX];
    if (!make_absolute(cwd, old_path, abs_old, sizeof(abs_old)) ||
        !make_absolute(cwd, new_path, abs_new, sizeof(abs_new))) {
        return 0;
    }
    normalize_path(abs_old);
    normalize_path(abs_new);
    return fat32_rename(abs_old, abs_new);
}
