/*
 * Not part of mlibc itself. glibc's <features.h> sets up its own
 * __USE_xxx / __GLIBC__ family of feature-test macros; a few busybox files
 * (e.g. libbb/makedev.c) include it unconditionally on any non-BSD/
 * non-Apple target purely so <sys/sysmacros.h> sees consistent feature
 * macros afterward -- nothing here actually branches on __GLIBC__ in a
 * way our build reaches (see makedev.c's own `#ifdef __GLIBC__` special
 * case, which we aren't), so an empty header (musl's own <features.h>
 * is likewise close to a no-op) is enough to satisfy the #include.
 */
#ifndef _FEATURES_H
#define _FEATURES_H
#endif
