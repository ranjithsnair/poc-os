/* Read-only USTAR (tar) archive reader -- the initrd. */
#ifndef TARFS_H
#define TARFS_H

#include <stdint.h>

/* Must be called once, with the base address and size of a loaded
 * ustar archive (a Limine module), before tarfs_read(). */
void tarfs_init(const uint8_t *archive, uint64_t archive_size);

/* Looks up `filename` in the archive. On success, returns a pointer
 * directly into the archive's own memory (no copy) and sets *out_size;
 * returns NULL if not found, or if tarfs_init() was never called. */
const uint8_t *tarfs_read(const char *filename, uint64_t *out_size);

#endif
