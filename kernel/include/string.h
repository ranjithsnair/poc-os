/* Freestanding replacements for the handful of libc mem/string builtins
 * clang can emit calls to on its own (struct copies, zero-init, etc.),
 * even though -ffreestanding means none of libc is linked in. */
#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif
