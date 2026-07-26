/*
 * In-memory read-write filesystem -- vfs.c's fallback writable backend
 * when there's no real disk to mount (fat32_init() found no virtio-blk
 * device, e.g. this live-CD ISO booted with nothing attached, or under a
 * hypervisor that doesn't emulate virtio-blk at all). Everything created
 * here lives in kmalloc'd memory and is gone at the next reboot -- that's
 * the deliberate tradeoff for working with zero disk setup; once PoC-OS
 * is actually installed to a real disk, fat32_init() succeeds instead and
 * vfs.c uses that (persistent) backend exclusively, never this one.
 *
 * Deliberately mirrors fat32.h's contract byte-for-byte -- same
 * struct fat32_file shape, same function signatures, same
 * fat32_dirent_status enum -- so vfs.c can dispatch to whichever backend
 * is active without either side knowing the other exists. `first_cluster`
 * is repurposed as a ramfs node-table index (not a real FAT cluster
 * number) and `dirent_cluster`/`dirent_offset` are always 0 -- both are
 * only ever interpreted by whichever one of fat32.c/ramfs.c produced the
 * handle, never compared across backends.
 *
 * Names are capped at 12 bytes (FAT32_NAME_MAX below), matching FAT32's
 * own 8.3 limit -- not because ramfs needs 8.3 formatting, but because
 * struct poc_dirent's name field (syscall.h) is a fixed 13-byte buffer
 * shared by both backends.
 */
#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include "fat32.h"

#define RAMFS_NAME_MAX 12

/* Resets to just an empty root directory. Call once at boot, only if
 * fat32_init() failed -- see vfs_init(). Cannot fail (no hardware to
 * probe, just static state). */
void ramfs_init(void);

/* Same contract as fat32_lookup(). */
int ramfs_lookup(const char *path, struct fat32_file *out);

/* Same contract as fat32_create(). */
int ramfs_create(const char *path, struct fat32_file *out);

/* Same contract as fat32_mkdir(). */
int ramfs_mkdir(const char *path);

/* Same contract as fat32_readdir() (including reusing its
 * fat32_dirent_status return values) -- ramfs has no on-disk slots to
 * mark deleted, so FAT32_DIRENT_SKIP is never actually returned, but
 * callers written against fat32_readdir() don't need to know that. */
int ramfs_readdir(struct fat32_file *dir, uint32_t index, char *name_out,
                   uint32_t *size_out, int *is_dir_out);

/* Same contract as fat32_unlink(). */
int ramfs_unlink(const char *path);

/* Same contract as fat32_rmdir(). */
int ramfs_rmdir(const char *path);

/* Same contract as fat32_rename(). */
int ramfs_rename(const char *old_path, const char *new_path);

/* Same contract as fat32_read(). */
int64_t ramfs_read(struct fat32_file *f, uint64_t offset, void *buf, uint64_t len);

/* Same contract as fat32_write(): grows the file, zero-filling any gap
 * if `offset` is past the current size. Never fails for being "out of
 * space" the way a real disk can -- only for a genuine kmalloc()
 * failure -- since it's backed by however much RAM the kernel heap has
 * left, not a fixed-size volume. */
int64_t ramfs_write(struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len);

#endif
