/*
 * Validated copies between a user process's address space and a kernel
 * buffer, for syscalls that take a pointer argument (SYS_WRITE_CHAR took
 * its argument by value specifically to avoid needing this -- see
 * syscall.c's original comment -- now that there's a real way to check a
 * user pointer before dereferencing it, buffer-based syscalls can exist).
 */
#ifndef USERCOPY_H
#define USERCOPY_H

#include <stdint.h>

/* Checks that every byte of [ptr, ptr+len) is mapped PRESENT|USER (and
 * WRITABLE too, if need_write) in address space pml4_phys, walking one
 * page at a time via vmm_translate() since the range may span several
 * non-contiguous physical frames. Returns 1 if the whole range is safe
 * to access, 0 otherwise (unmapped page, kernel-only page, or a
 * read-only page when need_write was requested). */
int user_range_ok(uint64_t pml4_phys, uint64_t ptr, uint64_t len, int need_write);

/* Copies len bytes from a user-space buffer into a kernel-side buffer,
 * after validating the whole range is present+user+readable. Returns 1
 * on success; on failure (bad range) kdst is left untouched and 0 is
 * returned. */
int copy_from_user(uint64_t pml4_phys, void *kdst, uint64_t user_src, uint64_t len);

/* Copies len bytes from a kernel-side buffer into a user-space buffer,
 * after validating the whole range is present+user+writable. Returns 1
 * on success, 0 on failure (nothing is written in that case). */
int copy_to_user(uint64_t pml4_phys, uint64_t user_dst, const void *ksrc, uint64_t len);

#endif
