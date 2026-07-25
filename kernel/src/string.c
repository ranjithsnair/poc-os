#include "string.h"

/* Copies `n` bytes from `src` to `dest`, one byte at a time.
 * Caller must guarantee the two ranges don't overlap -- if they might,
 * use memmove() below instead. */
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;         /* uint8_t* so "+ i" steps one byte at a time */
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

/* Fills the first `n` bytes of `dest` with the value `c` (only the
 * low 8 bits of `c` are used, matching the real memset()). */
void *memset(void *dest, int c, size_t n) {
    uint8_t *d = dest;
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dest;
}

/* Same job as memcpy(), but safe even when `dest` and `src` overlap.
 * The trick is choosing which end to start copying from:
 *   - if dest is before src, copy front-to-back (each byte is read
 *     before the copy could reach far enough to overwrite it)
 *   - if dest is after src, copy back-to-front instead, for the same
 *     reason in the other direction
 *   - if they're equal, there's nothing to do */
void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

/* Compares the first `n` bytes of `a` and `b`. Returns 0 if they're
 * identical, otherwise the (signed) difference between the first
 * pair of bytes that differ -- negative if `a`'s byte is smaller,
 * positive if it's larger. */
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] - pb[i];
        }
    }
    return 0;
}
