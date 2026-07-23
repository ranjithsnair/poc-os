/*
 * Minimal read-only USTAR (tar) archive reader, used to pull files out
 * of a Limine-loaded module (main.c's module_request) -- this is the
 * initrd: there's no disk driver or real filesystem yet, so any file
 * needed at boot has to be bundled directly into the boot ISO and
 * handed to the kernel as one in-memory blob.
 *
 * ustar was picked over a real on-disk filesystem (ext2, FAT32) purely
 * because its format is so much harder to get wrong: a flat sequence of
 * 512-byte headers (name/size/type as fixed-width ASCII/octal fields)
 * each immediately followed by that file's data, padded out to a
 * multiple of 512 bytes, ending at an all-zero block -- no inodes, no
 * block bitmaps, no indirection.
 *
 * The exact field layout and traversal logic here -- including the
 * "round data size up to the next 512-byte block" skip between entries
 * -- was cross-checked byte-for-byte against a real archive produced by
 * `tar --format=ustar` before being written here, not just derived from
 * the spec by hand.
 */
#include <stddef.h>
#include "tarfs.h"

#define TAR_BLOCK_SIZE 512
#define TAR_NAME_LEN 100

struct tar_header {
    char name[TAR_NAME_LEN];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed));

static const uint8_t *archive_base;
static uint64_t archive_size;

void tarfs_init(const uint8_t *archive, uint64_t size) {
    archive_base = archive;
    archive_size = size;
}

/* Parses a NUL- or space-terminated octal ASCII field (POSIX ustar). */
static uint64_t parse_octal(const char *field, size_t len) {
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c < '0' || c > '7') {
            break;
        }
        value = (value * 8) + (uint64_t)(c - '0');
    }
    return value;
}

static int is_zero_block(const uint8_t *block) {
    for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (block[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* header_name is a fixed TAR_NAME_LEN-byte field that (per POSIX ustar)
 * is NOT guaranteed to be NUL-terminated if it fills the whole field;
 * filename is an ordinary NUL-terminated C string. Reading filename[i]
 * for i up to and including TAR_NAME_LEN is always in-bounds: the loop
 * only reaches that point if filename's first TAR_NAME_LEN bytes were
 * all non-NUL, so it has at least one more byte (its terminator, or
 * more content) to read. */
static int names_equal(const char *header_name, const char *filename) {
    for (size_t i = 0; i < TAR_NAME_LEN; i++) {
        char h = header_name[i];
        char f = filename[i];
        if (h != f) {
            return 0;
        }
        if (f == '\0') {
            return 1;
        }
    }
    return filename[TAR_NAME_LEN] == '\0';
}

/* Shared by tarfs_read()/tarfs_stat(): finds filename's ustar header,
 * or NULL if the archive isn't initialized, malformed, or doesn't
 * contain it. */
static const struct tar_header *find_header(const char *filename) {
    if (archive_base == NULL) {
        return NULL;
    }

    uint64_t offset = 0;
    while (offset + TAR_BLOCK_SIZE <= archive_size) {
        const uint8_t *block = archive_base + offset;
        if (is_zero_block(block)) {
            break; /* end of archive */
        }

        const struct tar_header *hdr = (const struct tar_header *)block;
        /* Not a real ustar header -- either the end of the archive
         * (some writers omit the second all-zero block) or a build
         * misconfiguration (e.g. the Makefile's tar invocation dropping
         * --format=ustar). Either way there's nothing more to find. */
        if (hdr->magic[0] != 'u' || hdr->magic[1] != 's' || hdr->magic[2] != 't' ||
            hdr->magic[3] != 'a' || hdr->magic[4] != 'r') {
            break;
        }

        if (names_equal(hdr->name, filename)) {
            return hdr;
        }

        uint64_t size = parse_octal(hdr->size, sizeof(hdr->size));
        uint64_t data_blocks = (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        offset += TAR_BLOCK_SIZE + data_blocks * TAR_BLOCK_SIZE;
    }
    return NULL;
}

const uint8_t *tarfs_read(const char *filename, uint64_t *out_size) {
    const struct tar_header *hdr = find_header(filename);
    if (hdr == NULL) {
        return NULL;
    }
    *out_size = parse_octal(hdr->size, sizeof(hdr->size));
    return (const uint8_t *)hdr + TAR_BLOCK_SIZE;
}

const uint8_t *tarfs_stat(const char *filename, uint64_t *out_size, uint32_t *out_mode, char *out_typeflag) {
    const struct tar_header *hdr = find_header(filename);
    if (hdr == NULL) {
        return NULL;
    }
    *out_size = parse_octal(hdr->size, sizeof(hdr->size));
    *out_mode = (uint32_t)parse_octal(hdr->mode, sizeof(hdr->mode));
    *out_typeflag = hdr->typeflag;
    return (const uint8_t *)hdr + TAR_BLOCK_SIZE;
}
