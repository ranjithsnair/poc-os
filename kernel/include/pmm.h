/* Physical frame (4KiB page) allocator, built from Limine's memory map. */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "limine.h"

#define PMM_FRAME_SIZE 4096

/* Builds the frame bitmap from `memmap`. `hhdm_offset` is Limine's
 * higher-half direct map offset (limine_hhdm_response.offset) -- every
 * physical address this allocator touches is accessed through it, since
 * physical memory isn't otherwise mapped anywhere the kernel can reach. */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* Returns the physical address of a freshly allocated, zeroed frame, or
 * 0 if physical memory is exhausted (0 is never a valid allocation: it's
 * always inside the first page, which pmm_init() leaves marked used). */
uint64_t pmm_alloc_frame(void);

void pmm_free_frame(uint64_t phys_addr);

uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

#endif
