#include <stdint.h>
#include "usercopy.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"

/* vmm_translate() doesn't report the PTE's permission bits, only the
 * resulting physical address (or UINT64_MAX) -- it's built for the
 * "kernel already knows this address is fine, just needs the physical
 * page" case (e.g. process.c's stack construction). Validating an
 * arbitrary, syscall-supplied pointer needs the permission bits too, so
 * this walks the tables itself rather than reusing vmm_translate(). */
static uint64_t translate_checked(uint64_t pml4_phys, uint64_t virt, int need_write) {
    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_phys);
    size_t i4 = (virt >> 39) & 0x1FF;
    if (!(pml4[i4] & VMM_PRESENT) || !(pml4[i4] & VMM_USER)) return UINT64_MAX;

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[i4] & 0x000FFFFFFFFFF000ull);
    size_t i3 = (virt >> 30) & 0x1FF;
    if (!(pdpt[i3] & VMM_PRESENT) || !(pdpt[i3] & VMM_USER)) return UINT64_MAX;

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[i3] & 0x000FFFFFFFFFF000ull);
    size_t i2 = (virt >> 21) & 0x1FF;
    if (!(pd[i2] & VMM_PRESENT) || !(pd[i2] & VMM_USER)) return UINT64_MAX;

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[i2] & 0x000FFFFFFFFFF000ull);
    size_t i1 = (virt >> 12) & 0x1FF;
    uint64_t pte = pt[i1];
    if (!(pte & VMM_PRESENT) || !(pte & VMM_USER)) return UINT64_MAX;
    if (need_write && !(pte & VMM_WRITABLE)) return UINT64_MAX;

    return (pte & 0x000FFFFFFFFFF000ull) | (virt & 0xFFF);
}

/* Walks every page overlapping [ptr, ptr+len), one at a time, checking
 * each is actually mapped and accessible before any copy is allowed to
 * touch it -- see usercopy.h for exactly what "accessible" requires. */
int user_range_ok(uint64_t pml4_phys, uint64_t ptr, uint64_t len, int need_write) {
    if (len == 0) {
        return 1;
    }
    uint64_t start_page = ptr & ~(uint64_t)(PMM_FRAME_SIZE - 1);
    uint64_t end_page = (ptr + len - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
    for (uint64_t page = start_page; ; page += PMM_FRAME_SIZE) {
        if (translate_checked(pml4_phys, page, need_write) == UINT64_MAX) {
            return 0;
        }
        if (page == end_page) {
            break;
        }
    }
    return 1;
}

/* Shared by copy_from_user()/copy_to_user(): walks the range page by
 * page (each page may be a different, non-contiguous physical frame)
 * and hands the kernel-visible pointer for each page's overlap with
 * [ptr, ptr+len) to `copy_page`, which does the actual memcpy in
 * whichever direction the caller needs. */
typedef void (*page_copy_fn)(uint8_t *kernel_page_ptr, void *kbuf, uint64_t kbuf_offset, uint64_t n);

static int walk_user_range(uint64_t pml4_phys, uint64_t user_ptr, void *kbuf, uint64_t len,
                            int need_write, page_copy_fn fn) {
    if (!user_range_ok(pml4_phys, user_ptr, len, need_write)) {
        return 0;
    }

    uint64_t done = 0;
    while (done < len) {
        uint64_t va = user_ptr + done;
        uint64_t phys = translate_checked(pml4_phys, va, need_write);
        uint64_t page_off = va & (PMM_FRAME_SIZE - 1);
        uint64_t chunk = PMM_FRAME_SIZE - page_off;
        if (chunk > len - done) {
            chunk = len - done;
        }
        uint8_t *kernel_page_ptr = (uint8_t *)vmm_phys_to_virt(phys);
        fn(kernel_page_ptr, kbuf, done, chunk);
        done += chunk;
    }
    return 1;
}

/* Copies one chunk from a user page into the kernel buffer. */
static void copy_page_from_user(uint8_t *kernel_page_ptr, void *kbuf, uint64_t kbuf_offset, uint64_t n) {
    memcpy((uint8_t *)kbuf + kbuf_offset, kernel_page_ptr, n);
}

static void copy_page_to_user(uint8_t *kernel_page_ptr, void *kbuf, uint64_t kbuf_offset, uint64_t n) {
    /* kbuf here actually points at the (const-qualified) kernel source
     * buffer; walk_user_range()'s void* signature is shared with the
     * from-user direction, so the const is cast away only right here. */
    memcpy(kernel_page_ptr, (const uint8_t *)kbuf + kbuf_offset, n);
}

/* Reads `len` bytes out of a user process's memory into `kdst` (kernel
 * memory), e.g. for a syscall like write() that needs to see the bytes
 * the user process asked to write. */
int copy_from_user(uint64_t pml4_phys, void *kdst, uint64_t user_src, uint64_t len) {
    return walk_user_range(pml4_phys, user_src, kdst, len, 0, copy_page_from_user);
}

/* Writes `len` bytes from `ksrc` (kernel memory) into a user process's
 * memory, e.g. for a syscall like read() that needs to hand data back. */
int copy_to_user(uint64_t pml4_phys, uint64_t user_dst, const void *ksrc, uint64_t len) {
    return walk_user_range(pml4_phys, user_dst, (void *)ksrc, len, 1, copy_page_to_user);
}
