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

/* User-space address space layout, shared by every process (elf.c,
 * process.c). Both regions sit in the low (non-canonical-sign-extended)
 * half of the address space, far above any ELF's own PT_LOAD segments
 * (which link low, e.g. 0x400000) and far below the halfway point where
 * addresses would need the canonical high bits set. */
#define VMM_USER_STACK_TOP   0x0000700000000000ULL
#define VMM_USER_STACK_PAGES 8 /* 32KiB initial stack */
#define VMM_USER_ANON_BASE   0x0000600000000000ULL

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

/* Builds a brand-new address space that is a deep copy of `src_pml4_phys`:
 * the shared kernel/HHDM/heap entries are set up exactly as
 * vmm_create_address_space() does, and every present user-space leaf page
 * in the source is given a fresh physical frame with the same content and
 * the same permission flags (VMM_WRITABLE/VMM_USER/VMM_NX) in the new
 * address space. This is fork()'s copy-on-write-free "just duplicate
 * everything" primitive -- process_fork() uses it to give the child its
 * own private copy of the parent's entire address space. Returns 0 on
 * allocation failure (partial copies made so far are torn back down via
 * vmm_destroy_address_space() before returning). */
uint64_t vmm_clone_address_space(uint64_t src_pml4_phys);

/* Frees every physical frame this address space owns exclusively: a
 * full four-level page-table walk, freeing each present leaf frame via
 * pmm_free_frame(), skipping the three PML4 entries every address space
 * shares with the kernel's own (HHDM/kernel image/kernel heap -- see
 * vmm_create_address_space()) so shared kernel memory is never freed,
 * then freeing the intermediate tables and finally the PML4 frame
 * itself. Called from process_exit_current() so repeated process
 * creation/exit doesn't leak physical memory the way it used to. */
void vmm_destroy_address_space(uint64_t pml4_phys);

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

/* Returns the VMM_WRITABLE/VMM_USER/VMM_NX bits `virt` is currently
 * mapped with, or 0 if it isn't mapped -- elf.c uses this so that when
 * two PT_LOAD segments with different permissions (e.g. a read-only
 * header segment and an executable .text segment) share one physical
 * page, whichever is processed second can broaden the mapping (clear
 * NX, add WRITABLE) instead of silently re-narrowing it back down. */
uint64_t vmm_page_flags(uint64_t pml4_phys, uint64_t virt);

/* Converts a physical address to a kernel-accessible pointer via the
 * HHDM -- the only way to touch a physical frame (e.g. one fresh out of
 * pmm_alloc_frame()) before/without mapping it anywhere else. */
void *vmm_phys_to_virt(uint64_t phys);

#endif
