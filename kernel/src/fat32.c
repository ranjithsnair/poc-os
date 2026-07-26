/*
 * See fat32.h. Matches tools/mkfat32.py's on-disk layout exactly: every
 * BPB field this code depends on is *read* from the boot sector rather
 * than assumed, so the two only need to agree on the format, not on
 * exact sizes/offsets baked into both sides.
 *
 * Directory entries are the plain 32-byte short-name (8.3) form -- no
 * VFAT long-filename entries are read, written, or even skipped-over
 * specially (a real FAT32 driver has to skip LFN entries when scanning;
 * this one simply never encounters any, since mkfat32.py never writes
 * them and every name this driver itself creates is already 8.3).
 */
#include <stdint.h>
#include <stddef.h>
#include "fat32.h"
#include "virtio_blk.h"
#include "framebuffer.h"
#include "string.h"

#define DIRENT_SIZE 32
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20

#define FAT_EOC_MIN 0x0FFFFFF8u /* any value >= this marks end-of-chain */

struct fat_dirent_raw {
    uint8_t name[11];
    uint8_t attr;
    uint8_t reserved_nt;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_access_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed));

static uint16_t bytes_per_sector;
static uint8_t sectors_per_cluster;
static uint32_t reserved_sectors;
static uint8_t num_fats;
static uint32_t fat_size; /* sectors per FAT */
static uint32_t root_cluster;
static uint32_t data_start_lba;
static uint32_t total_clusters;
static uint32_t next_free_hint;
static int mounted = 0;

/* Reads a little-endian 16-bit value out of raw bytes (the on-disk boot
 * sector/directory entries are little-endian regardless of CPU). */
static uint16_t read_u16(const uint8_t *p, int off) {
    return (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
}

/* Same as read_u16(), but for a 32-bit value. */
static uint32_t read_u32(const uint8_t *p, int off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

/* FAT32 numbers clusters starting at 2 (0/1 are reserved), so cluster N's
 * data sits (N - 2) clusters into the data region -- converts that
 * cluster number into an absolute disk sector (LBA) to read/write. */
static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

/* The File Allocation Table (FAT) is one big array, one 32-bit entry per
 * cluster, forming a linked list: entry[N] holds the number of the
 * cluster that comes after cluster N in some file's chain (or a special
 * "end of chain" marker >= FAT_EOC_MIN). This reads entry `cluster`. */
static uint32_t fat_read_entry(uint32_t cluster) {
    uint8_t sector[512];
    uint32_t byte_off = cluster * 4;
    uint32_t lba = reserved_sectors + byte_off / bytes_per_sector;
    virtio_blk_read_sector(lba, sector);
    return read_u32(sector, (int)(byte_off % bytes_per_sector)) & 0x0FFFFFFFu;
}

/* Writes one FAT entry -- read-modify-write, since an entry is only 4 of
 * a 512-byte sector's bytes. */
static void fat_write_entry(uint32_t cluster, uint32_t value) {
    uint8_t sector[512];
    uint32_t byte_off = cluster * 4;
    uint32_t lba = reserved_sectors + byte_off / bytes_per_sector;
    virtio_blk_read_sector(lba, sector);
    uint32_t off = byte_off % bytes_per_sector;
    sector[off + 0] = (uint8_t)(value);
    sector[off + 1] = (uint8_t)(value >> 8);
    sector[off + 2] = (uint8_t)(value >> 16);
    sector[off + 3] = (uint8_t)(value >> 24);
    virtio_blk_write_sector(lba, sector);
    /* Single FAT (see tools/mkfat32.py) -- no second copy to mirror. */
}

/* Finds one free cluster (FAT entry == 0), marks it end-of-chain, and
 * returns its number, starting the search from next_free_hint (where the
 * last allocation left off) rather than from cluster 2 every time, so
 * repeated allocations don't rescan clusters already known to be used.
 * Returns 0 if the disk is full. */
static uint32_t alloc_cluster(void) {
    for (uint32_t i = 0; i < total_clusters; i++) {
        uint32_t c = 2 + ((next_free_hint - 2 + i) % total_clusters);
        if (fat_read_entry(c) == 0) {
            fat_write_entry(c, FAT_EOC_MIN);
            next_free_hint = c + 1;
            return c;
        }
    }
    return 0; /* out of space */
}

/* Walks a whole cluster chain from `cluster` to its end, marking every
 * cluster in it free (FAT entry = 0) -- used when deleting/truncating a
 * file so its clusters become available for reuse. */
static void free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        uint32_t next = fat_read_entry(cluster);
        fat_write_entry(cluster, 0);
        cluster = next;
    }
}

/* Fills every sector of `cluster` with zero bytes -- used for a freshly
 * allocated directory cluster (so its entries start "unused", 0x00) and
 * a freshly allocated file cluster (so unwritten bytes read back as 0). */
static void zero_cluster(uint32_t cluster) {
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    uint32_t lba = cluster_to_lba(cluster);
    for (uint8_t i = 0; i < sectors_per_cluster; i++) {
        virtio_blk_write_sector(lba + i, zero);
    }
}

/* Converts a single path component ("hello.txt") into its packed,
 * space-padded, uppercased 11-byte 8.3 form, matching
 * tools/mkfat32.py's short_name(). Returns 0 if it doesn't fit in 8.3. */
static int pack_short_name(const char *component, uint8_t out[11]) {
    uint64_t len = 0;
    int64_t dot = -1;
    for (const char *q = component; *q; q++, len++) {
        if (*q == '.') {
            dot = (int64_t)len;
        }
    }
    uint64_t base_len = (dot >= 0) ? (uint64_t)dot : len;
    const char *ext = (dot >= 0) ? (component + dot + 1) : (component + len);
    uint64_t ext_len = (dot >= 0) ? (len - (uint64_t)dot - 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return 0;
    }

    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    for (uint64_t i = 0; i < base_len; i++) {
        char c = component[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[i] = (uint8_t)c;
    }
    for (uint64_t i = 0; i < ext_len; i++) {
        char c = ext[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[8 + i] = (uint8_t)c;
    }
    return 1;
}

struct scan_result {
    uint32_t entry_cluster;
    uint32_t entry_offset;
    uint32_t first_cluster;
    uint32_t size;
    int is_dir;
};

/* Scans every entry in dir_cluster's chain for one matching name11.
 * Stops at the first all-zero name byte (per FAT32 convention, that
 * marks the end of the *entire* directory, not just this cluster).
 * Returns 1 and fills *out on a match, 0 if not found. */
static int scan_dir_for(uint32_t dir_cluster, const uint8_t name11[11], struct scan_result *out) {
    uint8_t sector[512];
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster; s++) {
            virtio_blk_read_sector(lba + s, sector);
            for (uint32_t e = 0; e < bytes_per_sector / DIRENT_SIZE; e++) {
                struct fat_dirent_raw *d = (struct fat_dirent_raw *)(sector + e * DIRENT_SIZE);
                if (d->name[0] == 0x00) {
                    return 0;
                }
                if (d->name[0] == 0xE5) {
                    continue;
                }
                if (memcmp(d->name, name11, 11) == 0) {
                    out->entry_cluster = cluster;
                    out->entry_offset = s * bytes_per_sector + e * DIRENT_SIZE;
                    out->first_cluster = ((uint32_t)d->first_cluster_hi << 16) | d->first_cluster_lo;
                    out->size = d->file_size;
                    out->is_dir = (d->attr & ATTR_DIRECTORY) != 0;
                    return 1;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return 0;
}

/* Finds the first unused slot (a 0x00 or 0xE5 name byte) in dir_cluster's
 * chain, extending the chain with one freshly zeroed cluster if every
 * existing cluster is full. Returns 1 and fills out_cluster/out_offset
 * on success, 0 if out of space. */
static int find_free_slot(uint32_t dir_cluster, uint32_t *out_cluster, uint32_t *out_offset) {
    uint8_t sector[512];
    uint32_t cluster = dir_cluster;
    uint32_t prev = 0;
    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster; s++) {
            virtio_blk_read_sector(lba + s, sector);
            for (uint32_t e = 0; e < bytes_per_sector / DIRENT_SIZE; e++) {
                uint8_t first_byte = sector[e * DIRENT_SIZE];
                if (first_byte == 0x00 || first_byte == 0xE5) {
                    *out_cluster = cluster;
                    *out_offset = s * bytes_per_sector + e * DIRENT_SIZE;
                    return 1;
                }
            }
        }
        prev = cluster;
        cluster = fat_read_entry(cluster);
    }
    uint32_t new_cluster = alloc_cluster();
    if (new_cluster == 0) {
        return 0;
    }
    zero_cluster(new_cluster);
    fat_write_entry(prev, new_cluster);
    *out_cluster = new_cluster;
    *out_offset = 0;
    return 1;
}

/* Writes a brand-new 32-byte directory entry at `cluster`/`offset`
 * (previously found empty by find_free_slot()), filling in the name,
 * attribute byte, starting cluster and size -- timestamps are left at 0
 * since this driver has no real-time clock to read them from. */
static void create_dirent_at(uint32_t cluster, uint32_t offset, const uint8_t name11[11],
                              uint8_t attr, uint32_t first_cluster, uint32_t size) {
    uint32_t lba = cluster_to_lba(cluster) + offset / bytes_per_sector;
    uint8_t sector[512];
    virtio_blk_read_sector(lba, sector);
    struct fat_dirent_raw *d = (struct fat_dirent_raw *)(sector + (offset % bytes_per_sector));
    memcpy(d->name, name11, 11);
    d->attr = attr;
    d->reserved_nt = 0;
    d->crt_time_tenth = 0;
    d->crt_time = 0;
    d->crt_date = 0;
    d->lst_access_date = 0;
    d->wrt_time = 0;
    d->wrt_date = 0;
    d->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    d->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    d->file_size = size;
    virtio_blk_write_sector(lba, sector);
}

/* Updates just the cluster/size fields of an existing entry (name/attr
 * untouched) -- what fat32_write() uses to persist growth. */
static void update_dirent(uint32_t cluster, uint32_t offset, uint32_t first_cluster, uint32_t size) {
    uint32_t lba = cluster_to_lba(cluster) + offset / bytes_per_sector;
    uint8_t sector[512];
    virtio_blk_read_sector(lba, sector);
    struct fat_dirent_raw *d = (struct fat_dirent_raw *)(sector + (offset % bytes_per_sector));
    d->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    d->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    d->file_size = size;
    virtio_blk_write_sector(lba, sector);
}

/* Converts a raw packed 8.3 name (11 bytes, space-padded, uppercase --
 * see pack_short_name()) back into a NUL-terminated "name.ext" string
 * (lowercased, purely for a friendlier directory listing -- fat32_lookup()
 * always uppercases a path component before comparing, so this doesn't
 * change what any lookup resolves to). Writes at most 13 bytes (8 + '.' +
 * 3 + NUL) to `out`. */
static void unpack_short_name(const uint8_t name11[11], char *out) {
    uint64_t len = 0;
    for (int i = 0; i < 8 && name11[i] != ' '; i++) {
        char c = (char)name11[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[len++] = c;
    }
    if (name11[8] != ' ') {
        out[len++] = '.';
        for (int i = 8; i < 11 && name11[i] != ' '; i++) {
            char c = (char)name11[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            out[len++] = c;
        }
    }
    out[len] = '\0';
}

/* Overwrites just the first name byte of an existing entry with 0xE5
 * (FAT32's "deleted" marker) -- what fat32_unlink()/fat32_rmdir()/
 * fat32_rename() use to remove an old entry without disturbing any
 * entry after it in the same directory (unlike zeroing it, which would
 * incorrectly look like "end of directory" to scan_dir_for()/
 * fat32_readdir() for every entry that follows). */
static void mark_dirent_deleted(uint32_t cluster, uint32_t offset) {
    uint32_t lba = cluster_to_lba(cluster) + offset / bytes_per_sector;
    uint8_t sector[512];
    virtio_blk_read_sector(lba, sector);
    sector[offset % bytes_per_sector] = 0xE5;
    virtio_blk_write_sector(lba, sector);
}

/* Splits an absolute path into its parent directory path (never empty --
 * "/" if the file is directly under root) and final component. Returns 0
 * if `path` has no '/' at all (fat32.c only ever deals in absolute
 * paths; making a relative one absolute is vfs.c's job). */
static int split_parent(const char *path, char *parent_out, uint64_t parent_cap, const char **name_out) {
    const char *last_slash = NULL;
    for (const char *q = path; *q; q++) {
        if (*q == '/') {
            last_slash = q;
        }
    }
    if (last_slash == NULL) {
        return 0;
    }
    uint64_t parent_len = (uint64_t)(last_slash - path);
    if (parent_len == 0) {
        if (parent_cap < 2) {
            return 0;
        }
        parent_out[0] = '/';
        parent_out[1] = '\0';
    } else {
        if (parent_len + 1 > parent_cap) {
            return 0;
        }
        memcpy(parent_out, path, parent_len);
        parent_out[parent_len] = '\0';
    }
    *name_out = last_slash + 1;
    return **name_out != '\0';
}

/* Mounts the FAT32 filesystem: brings up the underlying virtio-blk disk,
 * reads sector 0 (the "boot sector", which despite the name is really
 * just a header describing the filesystem's layout -- the BIOS Parameter
 * Block, or BPB), and pulls out the handful of fields (cluster size,
 * where the data region starts, etc.) every other function here needs. */
int fat32_init(void) {
    if (!virtio_blk_init()) {
        return 0;
    }

    uint8_t sector[512];
    if (virtio_blk_read_sector(0, sector) != 0) {
        fb_print("fat32: failed to read the boot sector.\n");
        return 0;
    }
    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA) {
        fb_print("fat32: bad boot sector signature -- not formatted?\n");
        return 0;
    }

    bytes_per_sector = read_u16(sector, 0x0B);
    if (bytes_per_sector != 512) {
        fb_print("fat32: unsupported bytes_per_sector (must be 512).\n");
        return 0;
    }
    sectors_per_cluster = sector[0x0D];
    reserved_sectors = read_u16(sector, 0x0E);
    num_fats = sector[0x10];
    fat_size = read_u32(sector, 0x24);
    root_cluster = read_u32(sector, 0x2C);
    uint32_t total_sectors = read_u32(sector, 0x20);

    data_start_lba = reserved_sectors + (uint32_t)num_fats * fat_size;
    total_clusters = (total_sectors - data_start_lba) / sectors_per_cluster;
    next_free_hint = 2;
    mounted = 1;

    fb_print("fat32: mounted, ");
    fb_print_dec(total_clusters);
    fb_print(" clusters of ");
    fb_print_dec((uint64_t)sectors_per_cluster * bytes_per_sector);
    fb_print(" bytes each.\n");
    return 1;
}

/* Walks `path` one "/"-separated component at a time, starting at the
 * root directory, looking each component up in turn (scan_dir_for())
 * and descending into it if it's a directory and there's more path left. */
int fat32_lookup(const char *path, struct fat32_file *out) {
    if (!mounted) {
        return 0;
    }

    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        out->first_cluster = root_cluster;
        out->size = 0;
        out->is_dir = 1;
        out->dirent_cluster = 0;
        out->dirent_offset = 0;
        return 1;
    }

    uint32_t parent_cluster = root_cluster;
    struct scan_result sr = {0, 0, 0, 0, 0};

    while (*p) {
        uint64_t len = 0;
        while (p[len] && p[len] != '/') {
            len++;
        }
        char comp[13];
        if (len == 0 || len >= sizeof(comp)) {
            return 0;
        }
        memcpy(comp, p, len);
        comp[len] = '\0';
        p += len;
        while (*p == '/') {
            p++;
        }

        uint8_t name11[11];
        if (!pack_short_name(comp, name11)) {
            return 0;
        }
        if (!scan_dir_for(parent_cluster, name11, &sr)) {
            return 0;
        }
        if (*p != '\0') {
            if (!sr.is_dir) {
                return 0; /* tried to descend into a file */
            }
            parent_cluster = sr.first_cluster;
        }
    }

    out->first_cluster = sr.first_cluster;
    out->size = sr.size;
    out->is_dir = sr.is_dir;
    out->dirent_cluster = sr.entry_cluster;
    out->dirent_offset = sr.entry_offset;
    return 1;
}

int fat32_create(const char *path, struct fat32_file *out) {
    if (!mounted) {
        return 0;
    }
    char parent_path[128];
    const char *name;
    if (!split_parent(path, parent_path, sizeof(parent_path), &name)) {
        return 0;
    }

    struct fat32_file parent;
    if (!fat32_lookup(parent_path, &parent) || !parent.is_dir) {
        return 0;
    }

    uint8_t name11[11];
    if (!pack_short_name(name, name11)) {
        return 0;
    }

    struct scan_result sr;
    if (scan_dir_for(parent.first_cluster, name11, &sr)) {
        if (sr.is_dir) {
            return 0; /* can't create a regular file over an existing directory */
        }
        if (sr.first_cluster != 0) {
            free_chain(sr.first_cluster);
        }
        update_dirent(sr.entry_cluster, sr.entry_offset, 0, 0);
        out->first_cluster = 0;
        out->size = 0;
        out->is_dir = 0;
        out->dirent_cluster = sr.entry_cluster;
        out->dirent_offset = sr.entry_offset;
        return 1;
    }

    uint32_t slot_cluster, slot_offset;
    if (!find_free_slot(parent.first_cluster, &slot_cluster, &slot_offset)) {
        return 0;
    }
    create_dirent_at(slot_cluster, slot_offset, name11, ATTR_ARCHIVE, 0, 0);
    out->first_cluster = 0;
    out->size = 0;
    out->is_dir = 0;
    out->dirent_cluster = slot_cluster;
    out->dirent_offset = slot_offset;
    return 1;
}

int fat32_mkdir(const char *path) {
    if (!mounted) {
        return 0;
    }
    char parent_path[128];
    const char *name;
    if (!split_parent(path, parent_path, sizeof(parent_path), &name)) {
        return 0;
    }

    struct fat32_file parent;
    if (!fat32_lookup(parent_path, &parent) || !parent.is_dir) {
        return 0;
    }

    uint8_t name11[11];
    if (!pack_short_name(name, name11)) {
        return 0;
    }

    struct scan_result sr;
    if (scan_dir_for(parent.first_cluster, name11, &sr)) {
        return 0; /* already exists */
    }

    uint32_t new_cluster = alloc_cluster();
    if (new_cluster == 0) {
        return 0;
    }
    zero_cluster(new_cluster);

    uint32_t slot_cluster, slot_offset;
    if (!find_free_slot(parent.first_cluster, &slot_cluster, &slot_offset)) {
        free_chain(new_cluster);
        return 0;
    }
    create_dirent_at(slot_cluster, slot_offset, name11, ATTR_DIRECTORY, new_cluster, 0);
    return 1;
}

int64_t fat32_read(struct fat32_file *f, uint64_t offset, void *buf, uint64_t len) {
    if (!mounted) {
        return -1;
    }
    if (offset >= f->size) {
        return 0;
    }
    uint64_t remaining_in_file = f->size - offset;
    if (len > remaining_in_file) {
        len = remaining_in_file;
    }
    if (len == 0) {
        return 0;
    }

    uint64_t cluster_size = (uint64_t)sectors_per_cluster * bytes_per_sector;
    uint64_t cluster_index = offset / cluster_size;
    uint64_t cluster_off = offset % cluster_size;

    uint32_t cluster = f->first_cluster;
    for (uint64_t i = 0; i < cluster_index; i++) {
        if (cluster < 2 || cluster >= FAT_EOC_MIN) {
            return -1;
        }
        cluster = fat_read_entry(cluster);
    }

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    uint8_t sector[512];
    while (done < len) {
        if (cluster < 2 || cluster >= FAT_EOC_MIN) {
            return (int64_t)done;
        }
        uint32_t lba = cluster_to_lba(cluster);
        while (cluster_off < cluster_size && done < len) {
            uint32_t sector_idx = (uint32_t)(cluster_off / bytes_per_sector);
            uint32_t sector_off = (uint32_t)(cluster_off % bytes_per_sector);
            virtio_blk_read_sector(lba + sector_idx, sector);
            uint64_t chunk = bytes_per_sector - sector_off;
            if (chunk > len - done) {
                chunk = len - done;
            }
            memcpy(out + done, sector + sector_off, chunk);
            done += chunk;
            cluster_off += chunk;
        }
        cluster_off = 0;
        cluster = fat_read_entry(cluster);
    }
    return (int64_t)done;
}

/* Writes into the file's cluster chain, walking/extending it one cluster
 * at a time as needed (allocating a fresh cluster whenever the write
 * reaches past the current end of the chain), then persists the new
 * size back to disk via update_dirent(). A write that only partially
 * overwrites a sector reads that sector first (see the "read-modify-
 * write" comment below) so the untouched bytes in it survive. */
int64_t fat32_write(struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len) {
    if (!mounted || len == 0) {
        return 0;
    }

    uint64_t cluster_size = (uint64_t)sectors_per_cluster * bytes_per_sector;

    if (f->first_cluster == 0) {
        uint32_t c = alloc_cluster();
        if (c == 0) {
            return -1;
        }
        zero_cluster(c);
        f->first_cluster = c;
    }

    uint64_t cluster_index = offset / cluster_size;
    uint32_t cluster = f->first_cluster;
    for (uint64_t i = 0; i < cluster_index; i++) {
        uint32_t next = fat_read_entry(cluster);
        if (next < 2 || next >= FAT_EOC_MIN) {
            uint32_t newc = alloc_cluster();
            if (newc == 0) {
                return -1;
            }
            zero_cluster(newc);
            fat_write_entry(cluster, newc);
            next = newc;
        }
        cluster = next;
    }

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    uint64_t cluster_off = offset % cluster_size;
    uint8_t sector[512];
    while (done < len) {
        uint32_t lba = cluster_to_lba(cluster);
        while (cluster_off < cluster_size && done < len) {
            uint32_t sector_idx = (uint32_t)(cluster_off / bytes_per_sector);
            uint32_t sector_off = (uint32_t)(cluster_off % bytes_per_sector);
            uint64_t chunk = bytes_per_sector - sector_off;
            if (chunk > len - done) {
                chunk = len - done;
            }
            if (chunk < bytes_per_sector) {
                virtio_blk_read_sector(lba + sector_idx, sector); /* partial sector: read-modify-write */
            }
            memcpy(sector + sector_off, in + done, chunk);
            virtio_blk_write_sector(lba + sector_idx, sector);
            done += chunk;
            cluster_off += chunk;
        }
        cluster_off = 0;
        if (done < len) {
            uint32_t next = fat_read_entry(cluster);
            if (next < 2 || next >= FAT_EOC_MIN) {
                uint32_t newc = alloc_cluster();
                if (newc == 0) {
                    break; /* out of space -- return the partial write below */
                }
                zero_cluster(newc);
                fat_write_entry(cluster, newc);
                next = newc;
            }
            cluster = next;
        }
    }

    uint64_t new_size = offset + done;
    if (new_size > f->size) {
        f->size = new_size;
    }
    update_dirent(f->dirent_cluster, f->dirent_offset, f->first_cluster, (uint32_t)f->size);
    return (int64_t)done;
}

int fat32_readdir(struct fat32_file *dir, uint32_t index, char *name_out,
                   uint32_t *size_out, int *is_dir_out) {
    if (!mounted || !dir->is_dir) {
        return FAT32_DIRENT_END;
    }

    uint32_t slots_per_cluster = (bytes_per_sector / DIRENT_SIZE) * sectors_per_cluster;
    uint32_t cluster_index = index / slots_per_cluster;
    uint32_t slot_in_cluster = index % slots_per_cluster;

    uint32_t cluster = dir->first_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        if (cluster < 2 || cluster >= FAT_EOC_MIN) {
            return FAT32_DIRENT_END;
        }
        cluster = fat_read_entry(cluster);
    }
    if (cluster < 2 || cluster >= FAT_EOC_MIN) {
        return FAT32_DIRENT_END;
    }

    uint32_t lba = cluster_to_lba(cluster) + (slot_in_cluster * DIRENT_SIZE) / bytes_per_sector;
    uint32_t byte_off = (slot_in_cluster * DIRENT_SIZE) % bytes_per_sector;
    uint8_t sector[512];
    virtio_blk_read_sector(lba, sector);
    struct fat_dirent_raw *d = (struct fat_dirent_raw *)(sector + byte_off);

    if (d->name[0] == 0x00) {
        return FAT32_DIRENT_END;
    }
    if (d->name[0] == 0xE5) {
        return FAT32_DIRENT_SKIP;
    }

    unpack_short_name(d->name, name_out);
    *size_out = d->file_size;
    *is_dir_out = (d->attr & ATTR_DIRECTORY) != 0;
    return FAT32_DIRENT_VALID;
}

int fat32_unlink(const char *path) {
    if (!mounted) {
        return 0;
    }
    struct fat32_file f;
    if (!fat32_lookup(path, &f) || f.is_dir) {
        return 0;
    }
    if (f.first_cluster != 0) {
        free_chain(f.first_cluster);
    }
    mark_dirent_deleted(f.dirent_cluster, f.dirent_offset);
    return 1;
}

int fat32_rmdir(const char *path) {
    if (!mounted) {
        return 0;
    }
    struct fat32_file f;
    if (!fat32_lookup(path, &f) || !f.is_dir || f.first_cluster == root_cluster) {
        return 0; /* not a directory, or the root directory itself */
    }

    if (f.first_cluster != 0) {
        uint8_t sector[512];
        uint32_t cluster = f.first_cluster;
        while (cluster >= 2 && cluster < FAT_EOC_MIN) {
            uint32_t lba = cluster_to_lba(cluster);
            for (uint8_t s = 0; s < sectors_per_cluster; s++) {
                virtio_blk_read_sector(lba + s, sector);
                for (uint32_t e = 0; e < bytes_per_sector / DIRENT_SIZE; e++) {
                    uint8_t first_byte = sector[e * DIRENT_SIZE];
                    if (first_byte != 0x00 && first_byte != 0xE5) {
                        return 0; /* not empty */
                    }
                }
            }
            cluster = fat_read_entry(cluster);
        }
        free_chain(f.first_cluster);
    }

    mark_dirent_deleted(f.dirent_cluster, f.dirent_offset);
    return 1;
}

int fat32_rename(const char *old_path, const char *new_path) {
    if (!mounted) {
        return 0;
    }
    struct fat32_file src;
    if (!fat32_lookup(old_path, &src) || src.first_cluster == root_cluster) {
        return 0; /* doesn't exist, or is the root directory */
    }

    char parent_path[128];
    const char *name;
    if (!split_parent(new_path, parent_path, sizeof(parent_path), &name)) {
        return 0;
    }
    struct fat32_file parent;
    if (!fat32_lookup(parent_path, &parent) || !parent.is_dir) {
        return 0;
    }
    uint8_t name11[11];
    if (!pack_short_name(name, name11)) {
        return 0;
    }

    struct scan_result existing;
    if (scan_dir_for(parent.first_cluster, name11, &existing)) {
        if (existing.entry_cluster == src.dirent_cluster && existing.entry_offset == src.dirent_offset) {
            return 1; /* renaming a path onto itself -- nothing to do */
        }
        if (existing.is_dir) {
            return 0; /* never overwrite a directory */
        }
        if (existing.first_cluster != 0) {
            free_chain(existing.first_cluster);
        }
        mark_dirent_deleted(existing.entry_cluster, existing.entry_offset);
    }

    uint32_t slot_cluster, slot_offset;
    if (!find_free_slot(parent.first_cluster, &slot_cluster, &slot_offset)) {
        return 0;
    }
    uint8_t attr = src.is_dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    create_dirent_at(slot_cluster, slot_offset, name11, attr, src.first_cluster, src.size);
    mark_dirent_deleted(src.dirent_cluster, src.dirent_offset);
    return 1;
}
