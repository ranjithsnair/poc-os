/* Force-included (-include) ahead of every coreutils source file - see
 * the build script. musl has no fpurge() (a BSD stdio extension
 * gnulib provides lib/fpurge.c for); declaring it here, after
 * <stdio.h>, lets src/system.h's write_error() typecheck. The real
 * definition (poc-os-specific, not gnulib's) lives in
 * coreutils/poc/coreutils_shims.c. */
#include <stdio.h>
#include <wchar.h>
extern int fpurge(FILE *);
/* SETLOCALE_NULL_MAX/setlocale_null_r: coreutils/lib/hard-locale.c (as
 * vendored here) uses both without including coreutils/lib/
 * setlocale_null.h itself - a gnulib-module-list/version mismatch in
 * this vendored snapshot, not a poc-os-specific gap: the real
 * implementation (coreutils/lib/setlocale_null.c) is unmodified
 * upstream gnulib and compiles and works as-is once this is visible. */
#define SETLOCALE_NULL_MAX (256+1)
extern int setlocale_null_r(int category, char *buf, size_t bufsize);
/* mbszero()/c32isprint(): see coreutils/poc/coreutils_shims.c for the
 * real (poc-os-specific) definitions - normally declared only in
 * gnulib's own <wchar.h> replacement (lib/wchar.in.h), which this
 * build bypasses in favor of musl's real <wchar.h>. */
extern void mbszero(mbstate_t *);
extern int c32isprint(wint_t);
extern int c32isblank(wint_t);
extern int c32iscntrl(wint_t);
/* c32width(): same story - gnulib's own <wchar.h> replacement is the
 * only place that normally declares it; coreutils/lib/c32width.c
 * itself is a real, complete implementation (needed by ls -l's column
 * width computation for multibyte filenames), just missing this one
 * declaration under the real musl <wchar.h> this build uses instead.
 * char32_t itself is musl's <uchar.h>, not <wchar.h>. */
#include <uchar.h>
extern int c32width(char32_t);

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

/* mempcpy(): same reasoning as memrchr() above - musl really does
 * implement it (musl/src/string/mempcpy.c, real symbol, already
 * linked in), just declared under #ifdef _GNU_SOURCE, which this
 * build's -D_XOPEN_SOURCE=700 doesn't define. */
extern void *mempcpy(void *, const void *, size_t);

/* lchmod(): same reasoning as memrchr()/mempcpy() above - musl really
 * does declare and implement it (musl/include/sys/stat.h, guarded by
 * #if defined(_GNU_SOURCE) || defined(_BSD_SOURCE), which this
 * build's -D_XOPEN_SOURCE=700 doesn't define; the real (poc-os-
 * specific) definition lives in coreutils/poc/coreutils_shims.c,
 * since poc-os's lchmod() is a no-op stub like chmod() itself, not
 * musl's - see that file's own comment). */
#include <sys/stat.h>
extern int lchmod(const char *, mode_t);

/* canonicalize_file_name(): a glibc extension musl doesn't provide at
 * all (musl only has the POSIX realpath()) - copy.c relies on
 * <stdlib.h> declaring it, the same way glibc's own does. The real
 * (poc-os-appropriate) definition is gnulib's own, in
 * coreutils/lib/canonicalize.c - already vendored, genuinely
 * implementing it in terms of realpath()-equivalent logic, not
 * something this port stands in for. */
extern char *canonicalize_file_name(const char *);

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

/* S_IRWXUGO: a Linux/glibc <bits/stat.h> macro ("S_IRWXU|S_IRWXG|
 * S_IRWXO", i.e. 0777) musl's own <sys/stat.h> doesn't define under
 * any exposure macro - mkdir.c uses it directly as the default mode
 * before umask is applied (real mkdir(1) always creates at 0777 and
 * lets the umask do the restricting, same as this expands to). */
#include <sys/stat.h>
#define S_IRWXUGO (S_IRWXU | S_IRWXG | S_IRWXO)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)

/* S_ISCTG/S_ISMPB/S_ISMPC/S_ISMPX/S_ISNWK/S_ISPORT/S_ISWHT: a handful
 * of legacy/exotic Unix file types (Cray "contiguous data", V7
 * "multiplexed" files, HP-UX "network special", Solaris doors, BSD
 * whiteouts) coreutils/lib/{c-file-type,filemode}.c check for - musl's
 * <sys/stat.h> has no st_mode bits for any of them (real systems don't
 * either, outside those specific historical Unixes); always false is
 * the correct answer for a filesystem (poc-os's own) that has none of
 * these file types at all, not an approximation. */
#define S_ISCTG(mode) 0
#define S_ISDOOR(mode) 0
#define S_ISMPB(mode) 0
#define S_ISMPC(mode) 0
#define S_ISMPX(mode) 0
#define S_ISNAM(mode) 0
#define S_ISNWK(mode) 0
#define S_ISOFD(mode) 0
#define S_ISOFL(mode) 0
#define S_ISPORT(mode) 0
#define S_ISWHT(mode) 0

/* libc_hidden_proto/libc_hidden_def/__glibc_likely/__glibc_unlikely/
 * __set_errno: glibc-internal-build-only macros that
 * coreutils/lib/malloc/scratch_buffer*.c (canonicalize.c's scratch-
 * buffer dependency - see coreutils/lib/malloc/scratch_buffer.gl.h's
 * own comment) expect some libc-config.h-like header to have already
 * defined, the way building genuinely *inside* glibc would. None of
 * them need a real implementation outside that context:
 * libc_hidden_proto/_def exist purely as a faster same-DSO call-path
 * optimization, moot here; __glibc_likely/unlikely are exactly
 * __builtin_expect with friendlier names; __set_errno is exactly
 * "errno = ...". */
#define libc_hidden_proto(name)
#define libc_hidden_def(name)
#define __glibc_likely(cond) __builtin_expect((cond), 1)
#define __glibc_unlikely(cond) __builtin_expect((cond), 0)
#include <errno.h>
#define __set_errno(val) (errno = (val))
#define __always_inline __attribute__((__always_inline__)) inline

/* timezone_t/tzalloc/tzfree/localtime_rz/mktime_z: declared only in
 * gnulib's own <time.h> replacement (lib/time.in.h), which - like
 * every other gnulib header replacement - this build skips in favor
 * of musl's real <time.h>, and musl's real <time.h> has no timezone_t
 * API at all (it's a NetBSD/glibc extension, not POSIX). Unlike
 * mbszero()/c32isprint()/etc above, this isn't a "no implementation
 * of any kind for this libc" case: coreutils/lib/time_rz.c is gnulib's
 * own portable implementation of the whole API (built specifically
 * for libcs without a native timezone_t), and compiles and works
 * as-is once it can see this typedef/these declarations - ls -l's
 * (coreutils/src/ls.c) timestamp-column formatting is what actually
 * needs them. struct tm_zone itself stays opaque here exactly as
 * lib/time.in.h's own declaration leaves it - only time_rz.c's own
 * translation unit (which includes lib/time-internal.h) ever looks
 * inside one. */
struct tm_zone;
typedef struct tm_zone *timezone_t;
extern timezone_t tzalloc(char const *);
extern void tzfree(timezone_t);
extern struct tm *localtime_rz(timezone_t, time_t const *restrict, struct tm *restrict);
extern time_t mktime_z(timezone_t, struct tm *);
