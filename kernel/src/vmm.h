/*
 * Virtual memory manager. Every address space (the kernel's own, and one
 * per process) is a separate PML4 that shares the same kernel/HHDM/heap
 * mappings -- Limine's tables already correctly map the kernel image and
 * identity-map all of physical memory at the HHDM offset, so a fresh
 * address space just copies those three PML4 entries wholesale rather
 * than rebuilding them, and is otherwise empty until a process maps its
 * own code/stack into it.
 */
#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define VMM_PRESENT  (1ull << 0)
#define VMM_WRITABLE (1ull << 1)
#define VMM_USER     (1ull << 2)
#define VMM_NX       (1ull << 63)

/* Virtual base of a region reserved for kernel use (the heap, for now).
 * PML4 index 300 sits well clear of both the kernel image (index 511)
 * and Limine's HHDM (index 256, assuming < 512GiB of RAM -- true for any
 * machine this kernel realistically runs on), so nothing Limine set up
 * can collide with mappings placed here. See the PML4-index derivation
 * comment in vmm.c. */
#define VMM_KERNEL_HEAP_PML4_INDEX 300
#define VMM_KERNEL_HEAP_BASE (0xFFFF000000000000ULL | ((uint64_t)VMM_KERNEL_HEAP_PML4_INDEX << 39))

void vmm_init(uint64_t hhdm_offset);

/* Physical address of the PML4 that was active when vmm_init() ran
 * (Limine's, extended with the kernel heap) -- the address space every
 * kernel-only mapping (the heap) belongs to, and the template every
 * per-process address space is cloned from. */
uint64_t vmm_kernel_address_space(void);

/* Allocates a fresh PML4 and copies in the kernel/HHDM/heap entries from
 * vmm_kernel_address_space(), so kernel code, physical memory access,
 * and kmalloc all keep working no matter which address space is loaded.
 * Everything else starts unmapped. Returns 0 on allocation failure. */
uint64_t vmm_create_address_space(void);

/* Loads CR3. */
void vmm_switch_address_space(uint64_t pml4_phys);

/* Maps one 4KiB page at virtual address `virt` to physical frame `phys`
 * (both must already be page-aligned) within the address space named by
 * `pml4_phys`, creating whatever intermediate page tables are missing
 * along the way (via pmm_alloc_frame()). */
void vmm_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmaps the 4KiB page at `virt` in the given address space, if it was
 * mapped. Does not free whatever physical frame it pointed at -- callers
 * that own that frame (e.g. the heap) are responsible for calling
 * pmm_free_frame(). */
void vmm_unmap(uint64_t pml4_phys, uint64_t virt);

/* Returns the physical address `virt` currently maps to in the given
 * address space (with the same low 12 bits as `virt`), or UINT64_MAX if
 * it isn't mapped. */
uint64_t vmm_translate(uint64_t pml4_phys, uint64_t virt);

/* Converts a physical address to a kernel-accessible pointer via the
 * HHDM -- the only way to touch a physical frame (e.g. one fresh out of
 * pmm_alloc_frame()) before/without mapping it anywhere else. */
void *vmm_phys_to_virt(uint64_t phys);

#endif
