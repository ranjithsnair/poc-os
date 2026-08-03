/* poc-os stand-in for the generated <malloc/scratch_buffer.gl.h> - a
 * real ./configure+make run would produce this by copying this exact
 * file (coreutils/lib/malloc/scratch_buffer.h, the real glibc
 * implementation coreutils/lib/scratch_buffer.h's own #include
 * expects) verbatim; this build doesn't run that step, so it's
 * vendored pre-generated instead, the same way as coreutils/lib/
 * obstack.h. The glibc-internal-build-only macros this file's
 * __always_inline/libc_hidden_proto/__glibc_likely (and its sibling
 * .c files' __glibc_unlikely/libc_hidden_def/__set_errno) need are
 * defined once, for every coreutils source file, in
 * coreutils/poc/poc_prelude.h - not repeated here.
 */
#ifndef _SCRATCH_BUFFER_H
#define _SCRATCH_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

/* Scratch buffer.  Must be initialized with scratch_buffer_init
   before its use.  */
struct scratch_buffer {
  void *data;    /* Pointer to the beginning of the scratch area.  */
  size_t length; /* Allocated space at the data pointer, in bytes.  */
  union { max_align_t __align; char __c[1024]; } __space;
};

/* Initializes *BUFFER so that BUFFER->data points to BUFFER->__space
   and BUFFER->length reflects the available space.  */
static inline void
scratch_buffer_init (struct scratch_buffer *buffer)
{
  buffer->data = buffer->__space.__c;
  buffer->length = sizeof (buffer->__space);
}

/* Deallocates *BUFFER (if it was heap-allocated).  */
static inline void
scratch_buffer_free (struct scratch_buffer *buffer)
{
  if (buffer->data != buffer->__space.__c)
    free (buffer->data);
}

/* Grow *BUFFER by some arbitrary amount.  The buffer contents is NOT
   preserved.  Return true on success, false on allocation failure (in
   which case the old buffer is freed).  On success, the new buffer is
   larger than the previous size.  On failure, *BUFFER is deallocated,
   but remains in a free-able state, and errno is set.  */
bool __libc_scratch_buffer_grow (struct scratch_buffer *buffer);
libc_hidden_proto (__libc_scratch_buffer_grow)

/* Alias for __libc_scratch_buffer_grow.  */
static __always_inline bool
scratch_buffer_grow (struct scratch_buffer *buffer)
{
  return __glibc_likely (__libc_scratch_buffer_grow (buffer));
}

/* Like __libc_scratch_buffer_grow, but preserve the old buffer
   contents on success, as a prefix of the new buffer.  */
bool __libc_scratch_buffer_grow_preserve (struct scratch_buffer *buffer);
libc_hidden_proto (__libc_scratch_buffer_grow_preserve)

/* Alias for __libc_scratch_buffer_grow_preserve.  */
static __always_inline bool
scratch_buffer_grow_preserve (struct scratch_buffer *buffer)
{
  return __glibc_likely (__libc_scratch_buffer_grow_preserve (buffer));
}

/* Grow *BUFFER so that it can store at least NELEM elements of SIZE
   bytes.  The buffer contents are NOT preserved.  Both NELEM and SIZE
   can be zero.  Return true on success, false on allocation failure
   (in which case the old buffer is freed, but *BUFFER remains in a
   free-able state, and errno is set).  It is unspecified whether this
   function can reduce the array size.  */
bool __libc_scratch_buffer_set_array_size (struct scratch_buffer *buffer,
					   size_t nelem, size_t size);
libc_hidden_proto (__libc_scratch_buffer_set_array_size)

/* Alias for __libc_scratch_set_array_size.  */
static __always_inline bool
scratch_buffer_set_array_size (struct scratch_buffer *buffer,
			       size_t nelem, size_t size)
{
  return __glibc_likely (__libc_scratch_buffer_set_array_size
			 (buffer, nelem, size));
}

#endif /* _SCRATCH_BUFFER_H */
