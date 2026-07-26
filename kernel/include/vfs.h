/*
 * Thin path-resolution and backend-dispatch layer: turns a possibly-
 * relative path plus a process's cwd into the single absolute-path form
 * fat32_lookup()/ramfs_lookup() (etc.) understand, applies SYS_OPEN's
 * O_CREAT/O_TRUNC semantics, and picks whichever one writable filesystem
 * is actually mounted -- fat32.c's real (persistent) disk if
 * fat32_init() found one, or ramfs.c's in-memory (non-persistent) one
 * otherwise (see vfs_init()). Exactly one is ever active at a time, so
 * this doesn't do real multi-filesystem mount-point dispatch -- it
 * exists so process.c's fd code doesn't have to know which backend is
 * live, on-disk format/node-table details on the other. */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include "fat32.h"

/* Must be called once at boot, before vfs_open()/vfs_mkdir(). Tries the
 * real disk (fat32_init()) first; if none is found, falls back to an
 * in-memory filesystem (ramfs_init()) so file-backed syscalls still work
 * with no disk attached at all -- at the cost of nothing persisting
 * across a reboot. Always returns 1: there is always at least the ramfs
 * fallback. */
int vfs_init(void);

/* Reads/writes/lists whatever backend vfs_init() picked -- same
 * contracts as fat32_read()/fat32_write()/fat32_readdir() (or ramfs.h's
 * mirror of them). process.c's fd code calls these instead of the
 * fat32_*()/ramfs_*() functions directly so it never needs to know
 * which backend a given fd's handle came from. */
int64_t vfs_read(struct fat32_file *f, uint64_t offset, void *buf, uint64_t len);
int64_t vfs_write(struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len);
int vfs_readdir(struct fat32_file *dir, uint32_t index, char *name_out, uint32_t *size_out, int *is_dir_out);

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
