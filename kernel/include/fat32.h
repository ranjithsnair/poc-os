/*
 * Read-write FAT32 driver over virtio_blk.h -- the writable disk
 * process-visible files live on (tarfs/the initrd stays read-only,
 * in-memory, and kernel-internal only). Matches exactly what
 * tools/mkfat32.py writes: 512-byte sectors, 4KiB (8-sector) clusters, a
 * single FAT, and short (8.3) names only -- there's no VFAT long-filename
 * support, so every path component this driver is asked to resolve or
 * create must already be 8.3-compatible.
 */
#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

/* A resolved file or directory: enough to read/write its data and to
 * find its own 32-byte directory entry again to persist a size/
 * first-cluster change (a fresh file starts with first_cluster == 0,
 * meaning "no cluster allocated yet" -- fat32_write() allocates one on
 * the first byte written). */
struct fat32_file {
    uint32_t first_cluster;
    uint32_t size;
    int is_dir;
    /* Where this file's own directory entry lives, so fat32_write() can
     * persist an updated size/first_cluster back to disk. */
    uint32_t dirent_cluster;
    uint32_t dirent_offset;
};

/* Reads the boot sector via virtio_blk.h and parses the BPB. Returns 1 on
 * success, 0 if there's no usable virtio-blk device or the boot sector
 * doesn't look like FAT32 (bad signature, etc.) -- callers must treat 0
 * as "no writable filesystem available" the same way a missing initrd
 * module already does. */
int fat32_init(void);

/* Resolves an absolute path ("/" component-separated, from the root
 * directory -- no "." or ".." support, and no relative paths: that's
 * vfs.c's job, one layer up) to the file or directory it names. Returns
 * 1 and fills *out on success, 0 if any component doesn't exist. */
int fat32_lookup(const char *path, struct fat32_file *out);

/* Creates a new, empty regular file at `path` (whose parent directory
 * must already exist), or truncates an already-existing one to size 0
 * (freeing its old cluster chain first). Returns 1 and fills *out on
 * success, 0 on failure (parent doesn't exist, name not 8.3-compatible,
 * out of directory-entry slots/clusters). */
int fat32_create(const char *path, struct fat32_file *out);

/* Creates a new, empty directory at `path`. Returns 1 on success, 0 on
 * failure (parent doesn't exist, already exists, name not
 * 8.3-compatible, out of clusters). */
int fat32_mkdir(const char *path);

/* fat32_readdir()'s per-call outcome: FAT32_DIRENT_END means the
 * directory has no entry at or past `index` (index 0 lands past the
 * last one); FAT32_DIRENT_SKIP means slot `index` used to hold an entry
 * that's since been deleted -- the directory isn't over, just try
 * index + 1; FAT32_DIRENT_VALID means `name_out`/`size_out`/`is_dir_out`
 * were filled in. */
enum fat32_dirent_status { FAT32_DIRENT_END = 0, FAT32_DIRENT_VALID = 1, FAT32_DIRENT_SKIP = 2 };

/* Reads the directory-entry slot at position `index` (0-based, counting
 * every 32-byte slot in `dir`'s cluster chain in order, including
 * deleted ones) into `name_out` (must have room for 13 bytes),
 * `size_out`, `is_dir_out`. `dir` must itself be a directory (from a
 * prior fat32_lookup()/the root). Callers walk a whole directory by
 * calling this with index = 0, 1, 2, ... until FAT32_DIRENT_END. */
int fat32_readdir(struct fat32_file *dir, uint32_t index, char *name_out,
                   uint32_t *size_out, int *is_dir_out);

/* Deletes the regular file at `path`: frees its cluster chain (if any)
 * and marks its directory entry deleted. Returns 1 on success, 0 on
 * failure (doesn't exist, or names a directory -- use fat32_rmdir()). */
int fat32_unlink(const char *path);

/* Deletes the empty directory at `path`: frees its (single, still-zeroed)
 * cluster and marks its directory entry deleted. Returns 1 on success, 0
 * on failure (doesn't exist, names a file, isn't empty, or is the root
 * directory). */
int fat32_rmdir(const char *path);

/* Moves/renames whatever's at `old_path` to `new_path` (parent directory
 * of `new_path` must already exist). If `new_path` already names a
 * regular file, it's deleted first (rename-over-existing-file
 * semantics); if it names a directory, this fails instead of merging or
 * overwriting. Returns 1 on success, 0 on failure. */
int fat32_rename(const char *old_path, const char *new_path);

/* Reads up to `len` bytes starting at `offset` into `buf`, clamped to
 * f->size (reading at/past EOF returns 0). Returns the number of bytes
 * read, or -1 on an I/O error. */
int64_t fat32_read(struct fat32_file *f, uint64_t offset, void *buf, uint64_t len);

/* Writes `len` bytes at `offset`, growing the file (allocating new
 * clusters, zero-filling any gap if `offset` is past the current size)
 * and persisting the new size/first_cluster to its directory entry as
 * needed. Returns the number of bytes written, or -1 on an I/O error or
 * out-of-clusters. */
int64_t fat32_write(struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len);

#endif
