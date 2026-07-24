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

int vfs_open(const char *cwd, const char *path, int flags, struct fat32_file *out) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }

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
    return fat32_mkdir(abs);
}
