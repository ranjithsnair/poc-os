/*
 * Kernel heap: a simple implicit free list over a region of virtual
 * memory that grows on demand. There's no separate "next" pointer --
 * every block (used or free) sits immediately after the previous one in
 * memory, so the next block's address is always
 * (this block + sizeof(header) + this block's size), and the list ends
 * wherever heap_end (the edge of what's currently mapped) does.
 *
 * The heap only ever grows (new pages get mapped in as needed); nothing
 * ever unmaps a page, even if every block on it is freed. Good enough
 * for a single, long-lived kernel address space -- reclaiming whole
 * pages back to the PMM would need to track per-page allocation counts,
 * which isn't worth it until something actually churns through memory.
 */
#include <stdint.h>
#include "heap.h"
#include "vmm.h"
#include "pmm.h"

#define HEAP_ALIGN 16

struct heap_block {
    size_t size; /* usable bytes following this header, not including it */
    int free;
};

static uint64_t heap_start;
static uint64_t heap_end; /* one past the last mapped byte */

/* Rounds `v` up to the next multiple of `align` (a power of two). */
static size_t align_up(size_t v, size_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Returns a pointer to the block right after `block` in memory (see the
 * file header comment: there's no stored "next" pointer, so this is
 * just block's header + however many bytes block's payload uses). */
static struct heap_block *block_next(struct heap_block *block) {
    return (struct heap_block *)((uint8_t *)block + sizeof(struct heap_block) + block->size);
}

/* Splits `block` into a `size`-byte used portion and, if enough room is
 * left over to be worth tracking as its own block, a free remainder.
 * Otherwise the whole block is left as-is (a little internal
 * fragmentation beats a free block too small to ever satisfy anything). */
static void split_block(struct heap_block *block, size_t size) {
    size_t remaining = block->size - size;
    if (remaining < sizeof(struct heap_block) + HEAP_ALIGN) {
        return;
    }
    struct heap_block *rest = (struct heap_block *)((uint8_t *)block + sizeof(struct heap_block) + size);
    rest->size = remaining - sizeof(struct heap_block);
    rest->free = 1;
    block->size = size;
}

/* Maps enough fresh pages at heap_end to grow the heap by at least
 * `min_bytes`, advancing heap_end. Returns 0 (leaving heap_end
 * untouched by the failed page) if physical memory runs out partway. */
static int expand_heap(size_t min_bytes) {
    uint64_t pages = (min_bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            return 0;
        }
        vmm_map(vmm_kernel_address_space(), heap_end, phys, VMM_WRITABLE);
        heap_end += PMM_FRAME_SIZE;
    }
    return 1;
}

/* Marks the heap as empty (no pages mapped yet) -- the first kmalloc()
 * call is what actually maps the first page, via expand_heap(). */
void heap_init(void) {
    heap_start = VMM_KERNEL_HEAP_BASE;
    heap_end = heap_start;
}

/* Allocates `size` bytes on the kernel heap. First scans the existing
 * free list for a block big enough to reuse; only if nothing fits does
 * it ask expand_heap() to map in fresh memory. */
void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size = align_up(size, HEAP_ALIGN);

    struct heap_block *block = (struct heap_block *)heap_start;
    while ((uint64_t)block < heap_end) {
        if (block->free && block->size >= size) {
            split_block(block, size);
            block->free = 0;
            return (void *)((uint8_t *)block + sizeof(struct heap_block));
        }
        block = block_next(block);
    }

    /* Nothing free (or big enough) already mapped: grow the heap and
     * place the allocation in the freshly mapped space. */
    uint64_t old_end = heap_end;
    if (!expand_heap(size + sizeof(struct heap_block))) {
        return NULL;
    }
    block = (struct heap_block *)old_end;
    block->size = heap_end - old_end - sizeof(struct heap_block);
    block->free = 1;
    split_block(block, size);
    block->free = 0;
    return (void *)((uint8_t *)block + sizeof(struct heap_block));
}

/* Frees a pointer previously returned by kmalloc(). The block's header
 * sits just before `ptr` in memory, which is how we find it again with
 * no separate bookkeeping table. */
void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - sizeof(struct heap_block));
    block->free = 1;

    /* Coalesce forward with however many free blocks follow directly, so
     * a long run of frees doesn't fragment the heap into pieces too
     * small to reuse. There's no backward coalescing -- that needs a way
     * to find the previous block (a boundary tag or doubly-linked list),
     * which this implicit list doesn't have. */
    struct heap_block *next = block_next(block);
    while ((uint64_t)next < heap_end && next->free) {
        block->size += sizeof(struct heap_block) + next->size;
        next = block_next(block);
    }
}
