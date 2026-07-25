/*
 * Physical frame allocator: a single bitmap, one bit per 4KiB frame,
 * built from Limine's memory map. Bit set = frame in use or not usable,
 * bit clear = free.
 *
 * The bitmap has to live somewhere in physical memory, and the only
 * physical memory this code can address before the kernel builds its own
 * page tables (pmm.c comes before any of that) is whatever Limine already
 * mapped for us: its HHDM (higher-half direct map) identity-maps all of
 * physical memory at a fixed virtual offset, so every physical address
 * here is accessed as (hhdm_base + phys), never dereferenced directly.
 */
#include <stddef.h>
#include "pmm.h"
#include "serial.h"

static uint8_t *bitmap;
static uint64_t bitmap_size_bytes;
static uint64_t total_frames;
static uint64_t used_frames;
static uint64_t hhdm_base;
static uint64_t search_hint; /* frame index the next alloc scan starts from */

/* Converts a physical address to a virtual one we can actually
 * dereference, via Limine's HHDM (see the file header comment above). */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_base);
}

/* Each frame gets one bit in `bitmap`. `frame / 8` picks the byte that
 * bit lives in, `frame % 8` picks which of its 8 bits. Marks a frame as
 * used by setting its bit to 1. */
static inline void bitmap_set(uint64_t frame) {
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

/* Marks a frame as free by clearing its bit back to 0. */
static inline void bitmap_clear(uint64_t frame) {
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

/* Returns 1 if a frame's bit is set (used), 0 if clear (free). */
static inline int bitmap_test(uint64_t frame) {
    return (bitmap[frame / 8] >> (frame % 8)) & 1;
}

/* Rounds `v` up to the next multiple of `align` (which must be a power
 * of two). E.g. align_up(4097, 4096) == 8192. */
static uint64_t align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Rounds `v` down to the previous multiple of `align`. */
static uint64_t align_down(uint64_t v, uint64_t align) {
    return v & ~(align - 1);
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    hhdm_base = hhdm_offset;

    /* Size the bitmap to cover every frame up to the end of the highest
     * memmap entry, so it can address any frame Limine describes at all
     * -- reserved/ACPI/MMIO gaps included, they just stay marked used. */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        uint64_t end = entry->base + entry->length;
        if (end > highest_addr) {
            highest_addr = end;
        }
    }
    total_frames = highest_addr / PMM_FRAME_SIZE;
    bitmap_size_bytes = (total_frames + 7) / 8;

    /* Find a USABLE region big enough to hold the bitmap itself, and
     * place it at that region's base. */
    bitmap = NULL;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size_bytes) {
            bitmap = (uint8_t *)phys_to_virt(entry->base);
            break;
        }
    }
    if (bitmap == NULL) {
        serial_print("PMM: no usable region large enough for the frame bitmap, halting.\n");
        for (;;) {
            asm volatile ("cli; hlt");
        }
    }

    /* Start pessimistic: everything used. USABLE regions get cleared
     * below, so anything Limine didn't explicitly hand us (reserved,
     * ACPI, the kernel/modules region, the framebuffer, ...) stays
     * marked used forever. */
    for (uint64_t i = 0; i < bitmap_size_bytes; i++) {
        bitmap[i] = 0xFF;
    }
    used_frames = total_frames;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        uint64_t start = align_up(entry->base, PMM_FRAME_SIZE);
        uint64_t end = align_down(entry->base + entry->length, PMM_FRAME_SIZE);
        for (uint64_t addr = start; addr < end; addr += PMM_FRAME_SIZE) {
            bitmap_clear(addr / PMM_FRAME_SIZE);
            used_frames--;
        }
    }

    /* Re-reserve the frames the bitmap occupies -- it lives inside a
     * USABLE region that the loop above just marked entirely free. */
    uint64_t bitmap_phys_base = (uint64_t)bitmap - hhdm_base;
    uint64_t bitmap_frames = align_up(bitmap_size_bytes, PMM_FRAME_SIZE) / PMM_FRAME_SIZE;
    for (uint64_t i = 0; i < bitmap_frames; i++) {
        uint64_t frame = bitmap_phys_base / PMM_FRAME_SIZE + i;
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
        }
    }

    /* Frame 0 is never handed out, full stop -- that keeps a physical
     * address of 0 an unambiguous "allocation failed" return, regardless
     * of whether firmware happened to describe the first page as usable. */
    if (!bitmap_test(0)) {
        bitmap_set(0);
        used_frames++;
    }

    search_hint = 0;

    serial_print("PMM: initialized.\n");
}

/* Finds one free frame, marks it used, zeroes it, and returns its
 * physical address. `search_hint` remembers where the last search left
 * off so repeated allocations don't rescan already-used frames from the
 * very beginning every time -- the scan still wraps around to frame 0
 * if it reaches the end without finding anything before `search_hint`. */
uint64_t pmm_alloc_frame(void) {
    for (uint64_t i = 0; i < total_frames; i++) {
        uint64_t frame = search_hint + i;
        if (frame >= total_frames) {
            frame -= total_frames;
        }
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
            search_hint = frame + 1;

            uint64_t phys = frame * PMM_FRAME_SIZE;
            uint64_t *virt = (uint64_t *)phys_to_virt(phys);
            for (uint64_t w = 0; w < PMM_FRAME_SIZE / sizeof(uint64_t); w++) {
                virt[w] = 0;
            }
            return phys;
        }
    }
    return 0; /* out of physical memory */
}

void pmm_free_frame(uint64_t phys_addr) {
    uint64_t frame = phys_addr / PMM_FRAME_SIZE;
    if (frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}

uint64_t pmm_total_frames(void) {
    return total_frames;
}

uint64_t pmm_free_frames(void) {
    return total_frames - used_frames;
}
