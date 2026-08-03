/* Force-included (-include) ahead of every coreutils source file - see
 * the build script. musl has no fpurge() (a BSD stdio extension
 * gnulib provides lib/fpurge.c for); declaring it here, after
 * <stdio.h>, lets src/system.h's write_error() typecheck. The real
 * definition (poc-os-specific, not gnulib's) lives in
 * coreutils/poc/coreutils_shims.c. */
#include <stdio.h>
#include <wchar.h>
extern int fpurge(FILE *);
/* mbszero()/c32isprint(): see coreutils/poc/coreutils_shims.c for the
 * real (poc-os-specific) definitions - normally declared only in
 * gnulib's own <wchar.h> replacement (lib/wchar.in.h), which this
 * build bypasses in favor of musl's real <wchar.h>. */
extern void mbszero(mbstate_t *);
extern int c32isprint(wint_t);
extern int c32isblank(wint_t);

/* rawmemchr(): a GNU libc extension musl's own <string.h> never
 * declares (musl doesn't implement it under any exposure macro) - the
 * real (poc-os-specific) definition lives in
 * coreutils/poc/coreutils_shims.c, same as the c32is.../mbszero
 * functions above. */
#include <string.h>
extern void *rawmemchr(const void *, int);

/* memrchr(): musl DOES implement this one for real (musl/src/string/
 * memrchr.c, already linked in - see MUSL_LDSO_OBJS) - it's just only
 * declared in musl's own <string.h> under #ifdef _GNU_SOURCE, which
 * this build's -D_XOPEN_SOURCE=700 doesn't define. Declaring it here
 * merely exposes the real symbol, not a reimplementation. */
extern void *memrchr(const void *, int, size_t);

/* _GL_ATTRIBUTE_FORMAT_PRINTF_{STANDARD,SYSTEM}: normally defined by
 * gnulib's own lib/stdio.in.h (its <stdio.h> replacement) - we use
 * musl's real <stdio.h> directly instead (see the build script's -I
 * order), so these never get defined. Pure compiler-attribute
 * boilerplate (which printf-format flavor GCC should type-check
 * against), not target-specific, so copied verbatim from
 * lib/stdio.in.h rather than reinvented. */
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
# define _GL_ATTRIBUTE_SPEC_PRINTF_STANDARD __gnu_printf__
#else
# define _GL_ATTRIBUTE_SPEC_PRINTF_STANDARD __printf__
#endif
#define _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD(formatstring_parameter, first_argument) \
  _GL_ATTRIBUTE_FORMAT ((_GL_ATTRIBUTE_SPEC_PRINTF_STANDARD, formatstring_parameter, first_argument))
#define _GL_ATTRIBUTE_FORMAT_PRINTF_SYSTEM(formatstring_parameter, first_argument) \
  _GL_ATTRIBUTE_FORMAT ((_GL_ATTRIBUTE_SPEC_PRINTF_STANDARD, formatstring_parameter, first_argument))

/* getprogname(): a BSD function poc-os's musl doesn't have - the
 * native (macOS) config.h this was adapted from says HAVE_GETPROGNAME
 * 1, which is true on macOS/BSD but not here. lib/error.c's only use
 * of it is interchangeable with gnulib's own program_name (see
 * lib/progname.c, already linked in - see the Makefile). */
#include "progname.h"
#define getprogname() program_name
/* _GL_ARG_NONNULL: normally from gnulib's lib/arg-nonnull.h, pulled
 * in transitively by whichever gnulib header a given source file
 * happens to include - some (e.g. lib/error.c) use it without
 * including it directly, relying on an earlier header (like the
 * gnulib <stdio.h> replacement we don't use) to have brought it in
 * first. Force-included here so it's always available regardless. */
#include "arg-nonnull.h"

/* O_BINARY: a Windows-only open() flag - gnulib's own <fcntl.h>
 * replacement (lib/fcntl.in.h) defines it to 0 on every non-Windows
 * target, which is exactly what leaving it undefined and defining it
 * here directly amounts to; poc-os is obviously never Windows. */
#define O_BINARY 0

/* ULLONG_WIDTH: a C23 <limits.h> width macro gnulib's own <limits.h>
 * replacement (lib/limits.in.h) computes generically from ULLONG_MAX
 * (via its _GL_INTEGER_WIDTH helper) - unsigned long long is always
 * 64 bits under the x86-64 SysV ABI this whole build already commits
 * to everywhere else (see e.g. mmu.h's PGSIZE/uintp assumptions), so
 * hardcoding the answer is exact, not an approximation. */
#define ULLONG_WIDTH 64

/* <limits.h>: gnulib's lib/xdectoint.c (via xdectoumax.c/xdectoimax.c)
 * uses INT_MAX without including it directly, relying on some earlier
 * header in a real ./configure'd build (likely gnulib's own <limits.h>
 * replacement, transitively pulled in by a header this build bypasses
 * in favor of musl's real one - see this file's own -I order comments
 * elsewhere) to have brought it in first. Force-included here so it's
 * always available regardless, the same reasoning as arg-nonnull.h
 * above. */
#include <limits.h>
