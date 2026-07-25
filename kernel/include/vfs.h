/*
 * Thin path-resolution layer over fat32.h: turns a possibly-relative
 * path plus a process's cwd into the single absolute-path form
 * fat32_lookup()/fat32_create() understand, and applies SYS_OPEN's
 * O_CREAT/O_TRUNC semantics. There's exactly one filesystem mounted
 * (fat32.c's), so this doesn't do multi-filesystem mount-point
 * dispatch -- it exists so process.c's fd code and fat32.c's on-disk
 * format code don't have to each know the other's conventions (cwd
 * handling on one side, 8.3/cluster-chain details on the other).
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include "fat32.h"

/* Must be called once at boot, before vfs_open()/vfs_mkdir(). Returns 1
 * on success, 0 if there's no writable disk available (see
 * fat32_init()) -- callers must treat 0 as "no filesystem", same as a
 * missing initrd module already means for tarfs. */
int vfs_init(void);

/* Resolves `path` against `cwd` (used as-is if `path` starts with '/';
 * otherwise joined as "cwd/path") and looks it up. If it doesn't exist
 * and `flags` has O_CREAT set, creates it (as an empty regular file); if
 * it does exist and `flags` has O_TRUNC set, truncates it first. Returns
 * 1 and fills *out on success, 0 on failure (no such file/directory and
 * O_CREAT wasn't set, path too long, or the underlying fat32_create()
 * failed). */
int vfs_open(const char *cwd, const char *path, int flags, struct fat32_file *out);

/* Same cwd-relative resolution as vfs_open(), then fat32_mkdir(). */
int vfs_mkdir(const char *cwd, const char *path);

/* Same cwd-relative resolution as vfs_open(), then fat32_unlink(). */
int vfs_unlink(const char *cwd, const char *path);

/* Same cwd-relative resolution as vfs_open(), then fat32_rmdir(). */
int vfs_rmdir(const char *cwd, const char *path);

/* Resolves both `old_path` and `new_path` against `cwd` (independently --
 * a relative `new_path` is not resolved relative to `old_path`'s
 * directory), then fat32_rename(). */
int vfs_rename(const char *cwd, const char *old_path, const char *new_path);

#endif
