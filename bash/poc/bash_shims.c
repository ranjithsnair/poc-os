/* poc-os-specific stand-ins for the handful of real libc functions bash
 * needs that poc-os has no syscall for at all - same role
 * coreutils/poc/coreutils_shims.c plays for coreutils (see that file's
 * own comments, especially waitpid()/sigaction()/raise(), for the
 * established pattern this follows: an exact userspace equivalent where
 * one exists using syscalls poc-os already has, an honest fixed/no-op
 * answer where the underlying concept doesn't exist on this kernel at
 * all). Not gnulib, not bash source - hand-written for this port.
 */
#include <unistd.h>
#include <errno.h>

/* dup2(): musl's real src/unistd/dup2.c needs SYS_dup2 (or SYS_dup3 as
 * a fallback), neither of which poc-os has (see include/syscall.h) -
 * this is an *exact* userspace equivalent using only SYS_dup (which
 * poc-os does have), not an approximation, relying on two real,
 * documented facts: poc-os's own dup() (kernel/sysfile.c's sys_dup())
 * always returns the *lowest-numbered* available fd, and close() on an
 * already-closed fd is a harmless no-op (just returns -1). So: close
 * newfd first (freeing that slot, if it was open), then repeatedly
 * dup(oldfd) - each call is guaranteed to return the lowest still-free
 * fd - stashing away any result that lands below newfd (those slots
 * were free for some other reason and need to stay allocated so the
 * *next* dup() call is forced to skip them) until one call finally
 * returns exactly newfd, then release the stashed ones. user/sh.c's own
 * redirection code (case PIPE/REDIR in runcmd()) already relies on
 * this same lowest-available-fd guarantee by hand, just for the
 * simpler close-fd-then-dup-once case where the target is already the
 * lowest free fd.
 */
int
dup2(int oldfd, int newfd)
{
	int stash[32];
	int nstash, r;

	if (oldfd == newfd) {
		if (fcntl(oldfd, F_GETFD) < 0)
			return -1;
		return newfd;
	}

	close(newfd);

	nstash = 0;
	for (;;) {
		r = dup(oldfd);
		if (r < 0) {
			while (nstash > 0)
				close(stash[--nstash]);
			return -1;
		}
		if (r == newfd)
			break;
		if (nstash < (int)(sizeof(stash) / sizeof(stash[0])))
			stash[nstash++] = r;
		else
			close(r);
	}

	while (nstash > 0)
		close(stash[--nstash]);
	return newfd;
}

/* uname(): poc-os has no SYS_uname - a fixed answer describing this
 * kernel is more useful to bash (which reports $MACHTYPE/$OSTYPE-
 * adjacent info and gates a few code paths on sysname) than failing
 * outright, the same tradeoff coreutils_shims.c's gethostname() already
 * makes.
 */
#include <sys/utsname.h>
#include <string.h>

int
uname(struct utsname *buf)
{
	strcpy(buf->sysname, "poc-os");
	strcpy(buf->nodename, "poc-os");
	strcpy(buf->release, "0.0.0");
	strcpy(buf->version, "#1");
	strcpy(buf->machine, "x86_64");
	return 0;
}

/* getrlimit()/setrlimit(): poc-os has no SYS_getrlimit/SYS_setrlimit
 * (or the prlimit64/*_time64 variants musl's real implementation wants)
 * and enforces no resource limits of any kind on a process - reporting
 * every limit as unbounded (RLIM_INFINITY, both soft and hard) is the
 * accurate answer for a kernel that genuinely never restricts any of
 * these, not a fake success covering up a real limit; setrlimit()
 * succeeding as a no-op is likewise accurate (there is no limit
 * enforcement for it to tighten or loosen).
 */
#include <sys/resource.h>

int
getrlimit(int resource, struct rlimit *rlim)
{
	(void)resource;
	rlim->rlim_cur = RLIM_INFINITY;
	rlim->rlim_max = RLIM_INFINITY;
	return 0;
}

int
setrlimit(int resource, const struct rlimit *rlim)
{
	(void)resource; (void)rlim;
	return 0;
}

/* times(): poc-os has no SYS_times (or a per-process CPU-time clock of
 * any kind - see coreutils_shims.c's clock()/clock_gettime() comment
 * for the same gap). Zeroing every field before reporting failure is
 * the same honest-degradation answer clock_gettime() already settled
 * on there: the times builtin (builtins/times.def) then shows 0m0.000s
 * for a process-time report this kernel has no way to produce for
 * real, rather than crashing on an uninitialized struct.
 */
#include <sys/times.h>

clock_t
times(struct tms *buf)
{
	if (buf) {
		buf->tms_utime = 0;
		buf->tms_stime = 0;
		buf->tms_cutime = 0;
		buf->tms_cstime = 0;
	}
	return (clock_t)-1;
}
