/*
 * Not part of mlibc itself. A few busybox files include the legacy
 * <malloc.h> unconditionally on any non-BSD/non-Apple target (e.g.
 * libbb/appletlib.c, "for mallopt") without actually calling any of its
 * glibc-specific extras (mallopt()/malloc_usable_size()/mallinfo()) in
 * our configured build -- musl's own <malloc.h> is just this same
 * compatibility wrapper for exactly this reason, so mirroring it here
 * (rather than declaring the extras themselves) is enough.
 */
#ifndef _MALLOC_H
#define _MALLOC_H

#include <stdlib.h>

#endif /* _MALLOC_H */
