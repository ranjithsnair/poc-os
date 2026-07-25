/*
 * virtio-blk driver (legacy virtio-pci transport) -- the writable disk
 * fat32.c mounts as the root filesystem. QEMU's default machine type
 * (i440fx/"pc", not q35) puts PCI devices directly on bus 0 with no PCIe
 * root ports in the way, so a plain bus-0 scan is enough to find it.
 */
#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

/* Scans PCI bus 0 for a virtio-blk device, negotiates no optional
 * features (the base read/write protocol needs none), sets up its one
 * virtqueue, and reads its capacity. Returns 1 on success, 0 if no
 * device was found or setup failed (logged either way) -- callers
 * (fat32.c) must treat 0 as "no writable disk available". */
int virtio_blk_init(void);

/* Sector count the device reports (512-byte sectors). Only meaningful
 * after a successful virtio_blk_init(). */
uint64_t virtio_blk_capacity_sectors(void);

/* Reads/writes exactly one 512-byte sector at `lba` into/from `buf`
 * (which must point at 512 bytes of kernel memory -- not necessarily
 * physically contiguous itself, since these go through an internal
 * bounce-buffer page so the actual DMA target is always a single known
 * physical frame). Returns 0 on success, -1 on failure (no device, or
 * the device reported an error status). Synchronous: busy-polls the
 * used ring rather than waiting for an interrupt, consistent with this
 * kernel having no other driver that needs virtio's interrupt path. */
int virtio_blk_read_sector(uint64_t lba, uint8_t *buf);
int virtio_blk_write_sector(uint64_t lba, const uint8_t *buf);

#endif
