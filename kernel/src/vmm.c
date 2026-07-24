/*
 * See vmm.h for the overall design (one PML4 per address space, sharing
 * kernel/HHDM/heap entries).
 *
 * PML4 index derivation for VMM_KERNEL_HEAP_BASE: a 4-level x86-64
 * virtual address splits into PML4:PDPT:PD:PT:offset as bits
 * [47:39]:[38:30]:[29:21]:[20:12]:[11:0], and the top 16 bits above bit
 * 47 must equal bit 47 itself (the "canonical address" rule) -- so
 * `0xFFFF000000000000 | (index << 39)` is the canonical address whose
 * PML4 index is exactly `index`, for any index in 0-511.
 *
 * KERNEL_PML4_INDEX is 511 because linker.ld links the kernel at
 * 0xffffffff80000000 (mcmodel=kernel's "top 2GB" convention), which
 * falls within the top-level PML4 entry covering the last 512GB of the
 * address space -- unlike the HHDM offset (Limine's choice, read at
 * runtime), the kernel's own link address is fixed at compile time, so
 * this doesn't need to be derived from anything Limine hands back.
 */
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"
#include "string.h"

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull
#define KERNEL_PML4_INDEX 511

#define IA32_EFER_MSR 0xC0000080u
#define EFER_NXE (1ull << 11)

static uint64_t hhdm_base;
static uint64_t kernel_pml4_phys; /* the address space active when vmm_init() ran */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* VMM_NX (bit 63 of a PTE) is only honored by the MMU if EFER.NXE is
 * set; otherwise that bit is simply reserved, and setting it (as
 * process.c does for every process's stack) would fault the first time
 * anything actually touches that page. Whether Limine already enabled
 * this isn't part of its spec, so set it explicitly -- a no-op if it
 * was on already. */
static void enable_nx(void) {
    wrmsr(IA32_EFER_MSR, rdmsr(IA32_EFER_MSR) | EFER_NXE);
}

static inline uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(phys + hhdm_base);
}

static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void flush_tlb_page(uint64_t virt) {
    asm volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Returns an HHDM-mapped pointer to the table `entry` points at,
 * allocating and zeroing a fresh table first if `entry` isn't present.
 * Intermediate tables are always PRESENT|WRITABLE|USER: permissions are
 * enforced at the leaf (final) entry, per the standard x86 paging
 * convention of ANDing permissions down the walk. */
static uint64_t *next_table(uint64_t *entry) {
    if (!(*entry & VMM_PRESENT)) {
        uint64_t frame = pmm_alloc_frame(); /* zeroed already */
        *entry = frame | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    }
    return phys_to_virt(*entry & PTE_ADDR_MASK);
}

void vmm_init(uint64_t hhdm_offset) {
    hhdm_base = hhdm_offset;
    kernel_pml4_phys = read_cr3() & PTE_ADDR_MASK;
    enable_nx();
}

uint64_t vmm_kernel_address_space(void) {
    return kernel_pml4_phys;
}

uint64_t vmm_create_address_space(void) {
    uint64_t new_phys = pmm_alloc_frame(); /* zeroed already */
    if (new_phys == 0) {
        return 0;
    }

    uint64_t *new_pml4 = phys_to_virt(new_phys);
    uint64_t *kernel_pml4 = phys_to_virt(kernel_pml4_phys);

    size_t hhdm_index = (hhdm_base >> 39) & 0x1FF;
    new_pml4[hhdm_index] = kernel_pml4[hhdm_index];
    new_pml4[KERNEL_PML4_INDEX] = kernel_pml4[KERNEL_PML4_INDEX];
    new_pml4[VMM_KERNEL_HEAP_PML4_INDEX] = kernel_pml4[VMM_KERNEL_HEAP_PML4_INDEX];

    return new_phys;
}

/* Every user-space virtual address this kernel ever hands out (ELF
 * PT_LOAD segments linked low, the stack at VMM_USER_STACK_TOP, anon
 * memory at VMM_USER_ANON_BASE) has PML4 index < 256 -- the "low half" of
 * the canonical address space -- so reconstructing a virtual address from
 * a bare PML4/PDPT/PD/PT index walk never needs the sign-extension a
 * high-half address would require. */
uint64_t vmm_clone_address_space(uint64_t src_pml4_phys) {
    uint64_t dst_phys = vmm_create_address_space();
    if (dst_phys == 0) {
        return 0;
    }

    uint64_t *src_pml4 = phys_to_virt(src_pml4_phys);
    size_t hhdm_index = (hhdm_base >> 39) & 0x1FF;

    for (size_t i4 = 0; i4 < 512; i4++) {
        if (i4 == hhdm_index || i4 == KERNEL_PML4_INDEX || i4 == VMM_KERNEL_HEAP_PML4_INDEX) {
            continue;
        }
        if (!(src_pml4[i4] & VMM_PRESENT)) {
            continue;
        }

        uint64_t *src_pdpt = phys_to_virt(src_pml4[i4] & PTE_ADDR_MASK);
        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(src_pdpt[i3] & VMM_PRESENT)) {
                continue;
            }

            uint64_t *src_pd = phys_to_virt(src_pdpt[i3] & PTE_ADDR_MASK);
            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(src_pd[i2] & VMM_PRESENT)) {
                    continue;
                }

                uint64_t *src_pt = phys_to_virt(src_pd[i2] & PTE_ADDR_MASK);
                for (size_t i1 = 0; i1 < 512; i1++) {
                    if (!(src_pt[i1] & VMM_PRESENT)) {
                        continue;
                    }

                    uint64_t src_frame = src_pt[i1] & PTE_ADDR_MASK;
                    uint64_t flags = src_pt[i1] & (VMM_WRITABLE | VMM_USER | VMM_NX);
                    uint64_t dst_frame = pmm_alloc_frame();
                    if (dst_frame == 0) {
                        vmm_destroy_address_space(dst_phys);
                        return 0;
                    }
                    memcpy(phys_to_virt(dst_frame), phys_to_virt(src_frame), PMM_FRAME_SIZE);

                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                     ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    vmm_map(dst_phys, virt, dst_frame, flags);
                }
            }
        }
    }

    return dst_phys;
}

/* Every leaf mapping this kernel ever creates (vmm_map()) is a plain
 * 4KiB page -- next_table() never sets the PS (huge-page) bit on an
 * intermediate entry -- so this walk never needs to special-case a 1GiB/
 * 2MiB leaf appearing at the PDPT/PD level. */
void vmm_destroy_address_space(uint64_t pml4_phys) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    size_t hhdm_index = (hhdm_base >> 39) & 0x1FF;

    for (size_t i4 = 0; i4 < 512; i4++) {
        if (i4 == hhdm_index || i4 == KERNEL_PML4_INDEX || i4 == VMM_KERNEL_HEAP_PML4_INDEX) {
            continue; /* shared with the kernel's own address space -- not ours to free */
        }
        if (!(pml4[i4] & VMM_PRESENT)) {
            continue;
        }

        uint64_t pdpt_phys = pml4[i4] & PTE_ADDR_MASK;
        uint64_t *pdpt = phys_to_virt(pdpt_phys);
        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & VMM_PRESENT)) {
                continue;
            }

            uint64_t pd_phys = pdpt[i3] & PTE_ADDR_MASK;
            uint64_t *pd = phys_to_virt(pd_phys);
            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & VMM_PRESENT)) {
                    continue;
                }

                uint64_t pt_phys = pd[i2] & PTE_ADDR_MASK;
                uint64_t *pt = phys_to_virt(pt_phys);
                for (size_t i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & VMM_PRESENT) {
                        pmm_free_frame(pt[i1] & PTE_ADDR_MASK);
                    }
                }
                pmm_free_frame(pt_phys);
            }
            pmm_free_frame(pd_phys);
        }
        pmm_free_frame(pdpt_phys);
    }

    pmm_free_frame(pml4_phys);
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    asm volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

void *vmm_phys_to_virt(uint64_t phys) {
    return (void *)phys_to_virt(phys);
}

void vmm_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);

    size_t i4 = (virt >> 39) & 0x1FF;
    size_t i3 = (virt >> 30) & 0x1FF;
    size_t i2 = (virt >> 21) & 0x1FF;
    size_t i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = next_table(&pml4[i4]);
    uint64_t *pd   = next_table(&pdpt[i3]);
    uint64_t *pt   = next_table(&pd[i2]);

    pt[i1] = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;
    flush_tlb_page(virt);
}

void vmm_unmap(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);

    size_t i4 = (virt >> 39) & 0x1FF;
    size_t i3 = (virt >> 30) & 0x1FF;
    size_t i2 = (virt >> 21) & 0x1FF;
    size_t i1 = (virt >> 12) & 0x1FF;

    if (!(pml4[i4] & VMM_PRESENT)) return;
    uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
    if (!(pdpt[i3] & VMM_PRESENT)) return;
    uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
    if (!(pd[i2] & VMM_PRESENT)) return;
    uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);

    pt[i1] = 0;
    flush_tlb_page(virt);
}

uint64_t vmm_translate(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);

    size_t i4 = (virt >> 39) & 0x1FF;
    size_t i3 = (virt >> 30) & 0x1FF;
    size_t i2 = (virt >> 21) & 0x1FF;
    size_t i1 = (virt >> 12) & 0x1FF;

    if (!(pml4[i4] & VMM_PRESENT)) return UINT64_MAX;
    uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
    if (!(pdpt[i3] & VMM_PRESENT)) return UINT64_MAX;
    uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
    if (!(pd[i2] & VMM_PRESENT)) return UINT64_MAX;
    uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
    if (!(pt[i1] & VMM_PRESENT)) return UINT64_MAX;

    return (pt[i1] & PTE_ADDR_MASK) | (virt & 0xFFF);
}

uint64_t vmm_page_flags(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);

    size_t i4 = (virt >> 39) & 0x1FF;
    size_t i3 = (virt >> 30) & 0x1FF;
    size_t i2 = (virt >> 21) & 0x1FF;
    size_t i1 = (virt >> 12) & 0x1FF;

    if (!(pml4[i4] & VMM_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
    if (!(pdpt[i3] & VMM_PRESENT)) return 0;
    uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
    if (!(pd[i2] & VMM_PRESENT)) return 0;
    uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
    if (!(pt[i1] & VMM_PRESENT)) return 0;

    return pt[i1] & (VMM_WRITABLE | VMM_USER | VMM_NX);
}
