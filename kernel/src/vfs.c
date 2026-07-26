/* See vfs.h. */
#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "fat32.h"
#include "ramfs.h"
#include "syscall.h"
#include "string.h"
#include "framebuffer.h"

#define VFS_PATH_MAX 192

/* Which backend every vfs_* call below dispatches to -- decided once at
 * boot by vfs_init() and never changed afterward (there's exactly one
 * writable filesystem mounted for the life of the kernel, same as
 * before this file had two backends to choose between at all). */
enum vfs_backend { VFS_BACKEND_NONE, VFS_BACKEND_FAT32, VFS_BACKEND_RAMFS };
static enum vfs_backend backend = VFS_BACKEND_NONE;

/* Real disk (fat32.c/virtio_blk.c) if one's attached and formatted --
 * that's the persistent choice, meant for once PoC-OS is actually
 * installed to a disk instead of just booted as a live CD. Falls back to
 * an in-memory filesystem (ramfs.c) otherwise, so SYS_OPEN's O_CREAT/
 * SYS_WRITE/etc. all still work with zero disk setup -- e.g. this ISO
 * booted directly with no disk attached at all, or under a hypervisor
 * that doesn't emulate virtio-blk -- at the cost of every change being
 * lost on the next reboot. Always succeeds: ramfs_init() has no hardware
 * to fail against, just static state. */
int vfs_init(void) {
    if (fat32_init()) {
        backend = VFS_BACKEND_FAT32;
        return 1;
    }
    fb_print("vfs: no disk found -- using an in-memory filesystem (changes will not persist across reboots).\n");
    ramfs_init();
    backend = VFS_BACKEND_RAMFS;
    return 1;
}

int64_t vfs_read(struct fat32_file *f, uint64_t offset, void *buf, uint64_t len) {
    return (backend == VFS_BACKEND_FAT32) ? fat32_read(f, offset, buf, len) : ramfs_read(f, offset, buf, len);
}

int64_t vfs_write(struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len) {
    return (backend == VFS_BACKEND_FAT32) ? fat32_write(f, offset, buf, len) : ramfs_write(f, offset, buf, len);
}

int vfs_readdir(struct fat32_file *dir, uint32_t index, char *name_out, uint32_t *size_out, int *is_dir_out) {
    return (backend == VFS_BACKEND_FAT32)
        ? fat32_readdir(dir, index, name_out, size_out, is_dir_out)
        : ramfs_readdir(dir, index, name_out, size_out, is_dir_out);
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

    int found = (backend == VFS_BACKEND_FAT32) ? fat32_lookup(abs, out) : ramfs_lookup(abs, out);
    if (found) {
        if ((flags & O_TRUNC) && !out->is_dir) {
            /* fat32_create()/ramfs_create() truncate an existing file. */
            return (backend == VFS_BACKEND_FAT32) ? fat32_create(abs, out) : ramfs_create(abs, out);
        }
        return 1;
    }
    if (flags & O_CREAT) {
        return (backend == VFS_BACKEND_FAT32) ? fat32_create(abs, out) : ramfs_create(abs, out);
    }
    return 0;
}

int vfs_mkdir(const char *cwd, const char *path) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);
    return (backend == VFS_BACKEND_FAT32) ? fat32_mkdir(abs) : ramfs_mkdir(abs);
}

int vfs_unlink(const char *cwd, const char *path) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);
    return (backend == VFS_BACKEND_FAT32) ? fat32_unlink(abs) : ramfs_unlink(abs);
}

int vfs_rmdir(const char *cwd, const char *path) {
    char abs[VFS_PATH_MAX];
    if (!make_absolute(cwd, path, abs, sizeof(abs))) {
        return 0;
    }
    normalize_path(abs);
    return (backend == VFS_BACKEND_FAT32) ? fat32_rmdir(abs) : ramfs_rmdir(abs);
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
    return (backend == VFS_BACKEND_FAT32) ? fat32_rename(abs_old, abs_new) : ramfs_rename(abs_old, abs_new);
}
