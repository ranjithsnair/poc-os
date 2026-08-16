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
#include <fcntl.h>
#include <string.h>

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

/* __sigsetjmp_tail(): real musl (musl/src/signal/sigsetjmp_tail.c,
 * tail-called in the ordinary C sense - "jmp __sigsetjmp_tail", not a
 * special calling convention - from musl/src/signal/x86_64/
 * sigsetjmp.s's own sigsetjmp()/__sigsetjmp()) saves/restores the
 * *current signal mask* into the jmp_buf via a raw SYS_rt_sigprocmask
 * poc-os doesn't have. poc-os has no signal delivery of any kind (see
 * sigaction()'s own comment in coreutils_shims.c) - there is no real
 * mask to save or restore, so this is the exact same "process every
 * signal-mask op as the empty set" answer sigprocmask() itself already
 * gives, not an approximation of a real mask this kernel doesn't have.
 * (Provided here instead of linking musl's own object, so musl/ itself
 * stays unmodified.)
 */
#include <setjmp.h>

int
__sigsetjmp_tail(sigjmp_buf jb, int ret)
{
	(void)jb;
	return ret;
}

/* __libc_current_sigrtmin()/__libc_current_sigrtmax(): musl's own
 * <signal.h> defines the SIGRTMIN/SIGRTMAX macros in terms of these
 * (querying the kernel's real reserved-signal range) - poc-os has no
 * signal delivery of any kind (see coreutils_shims.c's sigaction()),
 * so there's no real range to query; fixed 34/64 matches the standard
 * glibc/musl real-time-signal convention bash/poc/signames.h's own
 * hand-derived table already assumes.
 */
int
__libc_current_sigrtmin(void)
{
	return 34;
}

int
__libc_current_sigrtmax(void)
{
	return 64;
}

/* sleep(): musl's real src/unistd/sleep.c wants nanosleep() (no
 * SYS_nanosleep here) - poc-os's own native SYS_sleep (kernel/
 * sysproc.c's sys_sleep(), already what user/sh.c and every other
 * poc-os program actually sleeps with) takes a tick count, not
 * seconds; xv6's timer interrupt (and this port's, unchanged - see
 * kernel/lapic.c/kernel/trap.c) fires at the classic xv6 100Hz, so
 * seconds*100 is the real, correct tick count, not an approximation.
 */
#include "syscall.h"

unsigned
sleep(unsigned seconds)
{
	syscall(SYS_sleep, (long)seconds * 100);
	return 0;
}

/* time(): real musl (musl/src/time/time.c) wants __clock_gettime(),
 * which needs a SYS_clock_gettime poc-os doesn't have (see
 * coreutils_shims.c's own clock_gettime() stub, which reports the
 * same thing). Reporting the Unix epoch is the same honest
 * degradation already established there, not a separate approximation -
 * every timestamp already reads 1970-01-01 for exactly this reason.
 */
#include <time.h>

time_t
time(time_t *t)
{
	if (t)
		*t = 0;
	return 0;
}

/* __randname(): real musl (musl/src/temp/__randname.c, used by
 * mktemp()/mkstemp()/mkdtemp()) wants __clock_gettime() (see time()
 * above) and __pthread_self()->tid for entropy - poc-os has neither a
 * real clock nor real threads. getpid() (real: every poc-os process
 * has one) plus a per-process counter is enough real distinctness for
 * what every caller here actually needs (a name not already in use in
 * the target directory - mkstemp()'s own retry-on-EEXIST loop, still
 * linked from musl unmodified, is what actually guarantees that, the
 * same as on any real system), not cryptographic randomness.
 */
char *
__randname(char *template)
{
	static unsigned counter;
	unsigned long r;
	int i;

	r = (unsigned long)getpid() * 2654435761UL + counter++;
	for (i = 0; i < 6; i++, r >>= 5)
		template[i] = 'A' + (r & 15) + (r & 16) * 2;
	return template;
}

/* ttyname()/ttyname_r(): real musl (musl/src/unistd/ttyname_r.c) wants
 * /proc/self/fd/N + readlink() - poc-os has neither procfs nor
 * symlinks. poc-os has exactly one real terminal device - the console
 * user/init.c mknod()s at "/console" (see its own comment) - so
 * reporting that fixed path for any valid (isatty()-passing) fd is the
 * accurate answer, not a guess, the same tradeoff gethostname() in
 * coreutils_shims.c already makes.
 */
#include <errno.h>

int
ttyname_r(int fd, char *name, size_t size)
{
	static const char console[] = "/console";

	if (isatty(fd) == 0)
		return errno ? errno : ENOTTY;
	if (size < sizeof(console))
		return ERANGE;
	memcpy(name, console, sizeof(console));
	return 0;
}

char *
ttyname(int fd)
{
	static char buf[16];

	if (ttyname_r(fd, buf, sizeof(buf)) != 0)
		return NULL;
	return buf;
}

/* alarm()/setitimer(): poc-os has no SYS_alarm/SYS_setitimer and no
 * signal delivery to fire SIGALRM through even if it did (see
 * sigaction()'s own comment in coreutils_shims.c) - a timer that will
 * never actually fire is the honest answer, not a fake success masking
 * a real gap: every caller here (builtins/read.c's -t timeout,
 * lib/sh/ufuncs.c's falarm()) already has to tolerate a real system
 * where the timer request itself can fail (EINVAL, resource limits),
 * and copes by just not getting interrupted.
 */
#include <sys/time.h>

unsigned
alarm(unsigned seconds)
{
	(void)seconds;
	return 0;
}

int
setitimer(int which, const struct itimerval *restrict new,
          struct itimerval *restrict old)
{
	(void)which; (void)new;
	if (old) {
		old->it_interval.tv_sec = old->it_interval.tv_usec = 0;
		old->it_value.tv_sec = old->it_value.tv_usec = 0;
	}
	return 0;
}

/* select()/pselect(): poc-os has no SYS_select/SYS_pselect6 (or
 * SYS_ppoll backing - see include/syscall.h's own comment: the ppoll
 * number exists only for musl's "secure exec" internals, not a real,
 * general poll/select implementation) - and no non-blocking peek of
 * any kind on a poc-os fd (kernel/console.c's console read() and
 * kernel/pipe.c's pipe read() both just block until data or EOF).
 * Every caller reachable here (lib/sh/input_avail.c's speculative
 * "is there already buffered input" check, lib/sh/ufuncs.c's fsleep(),
 * lib/sh/timers.c's shtimer_select()) already treats "nothing ready"
 * as a safe, correct answer - it just means bash falls back to its
 * normal blocking read path, or a timed wait doesn't get to return
 * early - so reporting a timeout (0 fds ready) unconditionally is
 * honest, not a masked failure.
 */
#include <sys/select.h>

int
pselect(int nfds, fd_set *restrict rfds, fd_set *restrict wfds, fd_set *restrict efds,
        const struct timespec *restrict timeout, const sigset_t *restrict sigmask)
{
	(void)nfds; (void)rfds; (void)wfds; (void)efds; (void)timeout; (void)sigmask;
	return 0;
}

int
select(int nfds, fd_set *restrict rfds, fd_set *restrict wfds, fd_set *restrict efds,
       struct timeval *restrict timeout)
{
	(void)nfds; (void)rfds; (void)wfds; (void)efds; (void)timeout;
	return 0;
}

/* siglongjmp(): real musl (musl/src/signal/siglongjmp.c) restores the
 * saved signal mask (via sigsetjmp_tail's counterpart) before jumping -
 * poc-os has no real signal mask (see __sigsetjmp_tail()'s own comment
 * above), so this is exactly plain longjmp(), not a simplification of
 * real mask-restore behavior this kernel doesn't have anyway.
 */
_Noreturn void
siglongjmp(sigjmp_buf buf, int ret)
{
	longjmp(buf, ret);
}

/* getppid(): poc-os has no SYS_getppid (kernel/proc.c's struct proc
 * does track ->parent, so a real syscall could report this exactly -
 * a reasonable future addition - but until then, 1 (matching real
 * Unix's own reparent-to-init convention, and honestly usually true
 * here: init is PID 1 and every login shell's real parent) is a
 * better answer than failing outright for $PPID/lib/sh/random.c's
 * genseed() (which only wants *some* distinguishing entropy input,
 * not a specific value).
 */
pid_t
getppid(void)
{
	return 1;
}

/* getgroups()/setgid()/setuid(): poc-os has no user/group accounts or
 * permission model of any kind (every process is implicitly uid/gid 0 -
 * see geteuid()/getuid() in coreutils_shims.c) - getgroups() reporting
 * zero supplementary groups and setgid()/setuid() succeeding as a
 * no-op are both the accurate answer, not stand-ins for a real
 * mechanism this kernel doesn't have.
 */
#include <sys/types.h>

int
getgroups(int size, gid_t list[])
{
	(void)size; (void)list;
	return 0;
}

int
setgid(gid_t gid)
{
	(void)gid;
	return 0;
}

int
setuid(uid_t uid)
{
	(void)uid;
	return 0;
}

/* getdtablesize(): poc-os's per-process open-file table (kernel/
 * proc.h's struct proc.ofile) is a fixed NOFILE (include/param.h) 16
 * entries - a real, fixed answer, not a guess. (Not #include
 * "param.h" directly: bash_shims.c compiles against musl's own
 * include search path - see MUSL_PIC_INC in the Makefile - which
 * doesn't reach poc-os's own include/ tree.)
 */
int
getdtablesize(void)
{
	return 16;
}

/* getrusage(): poc-os has no SYS_getrusage (or any of the per-process
 * CPU-time/page-fault/etc accounting it would report - see times()'s
 * own comment in bash_prelude.h/this file). Zeroing the whole struct
 * before reporting success is what builtins/times.def's only caller
 * here actually needs: a defined, safe answer instead of whatever
 * garbage was on the stack, the same tradeoff clock_gettime() in
 * coreutils_shims.c already makes for a real gap this kernel has no
 * way to fill.
 */
#include <sys/resource.h>
#include <string.h>

int
getrusage(int who, struct rusage *usage)
{
	(void)who;
	memset(usage, 0, sizeof(*usage));
	return 0;
}

/* isnetconn(): poc-os has no network stack of any kind (no socket()/
 * connect(), see redir.c's own HAVE_NETWORK gate for /dev/tcp - this
 * call site in shell.c, deciding whether stdin looks like a remote
 * login shell, isn't gated the same way) - stdin is never a network
 * connection here, so 0 is the accurate answer, not an approximation.
 */
int
isnetconn(int fd)
{
	(void)fd;
	return 0;
}

/* endpwent(): poc-os has no user/group database of any kind (see
 * getpwnam()/getpwuid() in coreutils_shims.c, always NULL) - nothing
 * was ever opened for lib/tilde/tilde.c's ~user enumeration to close,
 * so this is a real, correct no-op, not a stub masking a missing
 * implementation.
 */
void
endpwent(void)
{
}

/* sysconf(): real musl (musl/src/conf/sysconf.c) is a big generic
 * table covering every POSIX _SC_* name, several branches of which
 * (e.g. _SC_NPROCESSORS_ONLN) hard-code a SYS_sched_getaffinity call
 * poc-os has no number for - a real compile error, not a link gap,
 * since __syscall()'s SYS_* argument has to be a compile-time
 * constant. Only four _SC_* names are ever actually queried anywhere
 * in this port (bash/lib/sh/clktck.c, bash/lib/sh/oslib.c - grep
 * confirmed it), so a small, real answer for exactly those, backed by
 * this kernel's own real fixed limits (NOFILE from include/param.h -
 * see getdtablesize() above - and the classic xv6 100Hz timer this
 * port's sleep() above already relies on), is honest and complete for
 * every caller that exists, not a subset masquerading as the general
 * case.
 */
#include <unistd.h>

long
sysconf(int name)
{
	switch (name) {
	case _SC_CLK_TCK:
		return 100;
	case _SC_OPEN_MAX:
		return 16;
	case _SC_NGROUPS_MAX:
		return 0;
	case _SC_CHILD_MAX:
		return -1;	/* no fixed limit poc-os enforces */
	default:
		errno = EINVAL;
		return -1;
	}
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
