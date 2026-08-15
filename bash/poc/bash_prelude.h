/* Force-included (-include) ahead of every bash/lib source file - same
 * role coreutils/poc/poc_prelude.h plays for coreutils (see that file's
 * own comment). Bash itself (unlike coreutils/gnulib) already ships its
 * own config.h-driven portability layer (lib "sh" helpers), so this stays
 * much smaller: just the handful of real gaps bash_shims.c fills that
 * need a declaration visible before bash's own headers see them.
 */
#include <sys/types.h>

/* uname()/struct utsname: poc-os has no SYS_uname (musl's real
 * src/misc/uname.c wants one) - the real (poc-os-specific) definition
 * lives in bash_shims.c. <sys/utsname.h> already declares both for any
 * bash source that includes it directly; nothing else needed here.
 */

/* getrlimit()/setrlimit(): poc-os has no SYS_getrlimit/SYS_setrlimit
 * (or the *_time64/prlimit64 variants musl's real implementation wants) -
 * the real (poc-os-specific) definitions live in bash_shims.c, declared
 * by <sys/resource.h> already for any bash source that includes it.
 */

/* times(): poc-os has no SYS_times - the real (poc-os-specific)
 * definition lives in bash_shims.c, declared by <sys/times.h> already.
 */
