/* timespec_cmp(): coreutils/lib/timespec.h declares this (among
 * others) as a plain C99 "inline" function (_GL_INLINE expands to
 * bare "inline", not "static inline", under this build's config.h
 * branch - see its own #define), which needs exactly one real,
 * externally-linked definition to exist somewhere, the same C99 rule
 * chmodat()/chownat()/psame_inode() already needed a real
 * instantiation for (see coreutils_shims.c's own comments on those).
 * Real gnulib provides that instantiation via coreutils/lib/
 * timespec.c (#define _GL_TIMESPEC_INLINE _GL_EXTERN_INLINE then
 * #include "timespec.h"), but that forces EVERY inline function in
 * timespec.h to instantiate, including timespectod() - which returns
 * a `double` and needs the FPU (SSE), unavailable under this build's
 * -mgeneral-regs-only (see the Makefile's COREUTILS_PIC_CFLAGS
 * comment) for the same reason gnulib's hash.c and human.c needed
 * their own from-scratch reimplementations. ls -l is the only caller
 * that needs timespec_cmp() (for sorting/comparing file timestamps)
 * and never calls timespectod() at all, so providing just this one
 * function - real, exact, integer-only, matching timespec.h's own
 * formula exactly rather than approximating it - sidesteps the
 * problem instead of working around it.
 */
#include <time.h>

int
timespec_cmp(struct timespec a, struct timespec b)
{
	int sec_cmp = (a.tv_sec > b.tv_sec) - (a.tv_sec < b.tv_sec);
	int nsec_cmp = (a.tv_nsec > b.tv_nsec) - (a.tv_nsec < b.tv_nsec);
	return 2 * sec_cmp + nsec_cmp;
}
