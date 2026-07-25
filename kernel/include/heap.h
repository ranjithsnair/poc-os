/* Kernel heap: general-purpose dynamic allocation (kmalloc/kfree). */
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/* Must be called once, after vmm_init(), before the first kmalloc(). */
void heap_init(void);

/* Returns a pointer to at least `size` bytes, 16-byte aligned, or NULL if
 * physical memory is exhausted. Uninitialized, like libc's malloc. */
void *kmalloc(size_t size);

void kfree(void *ptr);

#endif
