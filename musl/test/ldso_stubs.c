/* poc-os-specific stand-ins for a handful of real musl internals that
 * musl/ldso/dynlink.c references but that only matter on dlopen()'s
 * runtime-loading path (see install_new_tls() and dlopen() itself) -
 * never on the ordinary static-linked-at-startup path this port's
 * PT_INTERP smoke test (musl/test/interp_main.c) actually exercises.
 * Real musl gets these from src/thread/lock_ptc.c (a pthread_rwlock_t)
 * and src/signal/block.c (SYS_rt_sigprocmask) - poc-os has neither
 * real rwlocks nor any signal syscall at all yet, and is single-
 * threaded regardless, so no-op bodies are correct for now, not just
 * expedient: nothing can race a dlopen() that never runs. __tl_lock/
 * __tl_unlock mirror real musl's own fallback (src/linux/membarrier.c
 * weak-aliases both to a no-op when threading support isn't linked
 * in) rather than inventing a new convention.
 *
 * Once a real coreutils build actually calls dlopen() (most don't),
 * these need real implementations - a real pthread_rwlock_t for the
 * ptc functions, and a real SYS_rt_sigprocmask (kernel/sysproc.c has
 * none yet) for the sig-blocking pair.
 */

void __inhibit_ptc(void) {}
void __acquire_ptc(void) {}
void __release_ptc(void) {}
void __block_app_sigs(void *set) { (void)set; }
void __restore_sigs(void *set) { (void)set; }
void __tl_lock(void) {}
void __tl_unlock(void) {}
int __membarrier(int cmd, int flags) { (void)cmd; (void)flags; return 0; }

// readlink(): replaces musl/src/unistd/readlink.c, which issues a raw
// SYS_readlinkat poc-os has no number for. The only caller that
// matters here is dynlink.c's ldso_dirname()/$ORIGIN resolution,
// always against the fixed path "/proc/self/exe" - poc-os has no
// procfs at all, so this always reporting ENOENT is not a
// simplification of real behavior, it's what a real readlink() would
// also report against a /proc/self/exe that doesn't exist. dynlink.c
// already treats ENOENT as "no $ORIGIN available" and falls back
// gracefully (see the "case ENOENT:" around dynlink.c's ldso_dirname()).
#include <errno.h>

long
readlink(const char *path, char *buf, unsigned long bufsize)
{
	(void)path; (void)buf; (void)bufsize;
	errno = ENOENT;
	return -1;
}

// __fstat()/fstat(): replaces musl/src/stat/fstat.c + fstatat.c, which
// assume a real Linux statx/newfstatat syscall poc-os has neither the
// syscall number for nor (see include/stat.h) a matching struct stat
// layout to translate from - poc-os's own struct stat is a handful of
// xv6-native fields (type/dev/ino/nlink/size), nothing like POSIX's.
// musl/ldso/dynlink.c's load_library() is the only caller that
// matters here, and only ever inspects st_dev/st_ino (to recognize an
// already-loaded library opened again by a different path) - both of
// which poc-os's native SYS_fstat (invoked directly below, bypassing
// musl's own fstat entirely) already reports, so this is a real
// translation, not a fake success stub.
#include "syscall_arch.h"
#include "bits/syscall.h.in"
#include <sys/stat.h>

struct poc_stat { short type; int dev; unsigned ino; short nlink; unsigned size; };

int
__fstat(int fd, struct stat *st)
{
	struct poc_stat pst;

	if (__syscall2(__NR_fstat, fd, (long)&pst) < 0)
		return -1;
	__builtin_memset(st, 0, sizeof *st);
	st->st_dev = pst.dev;
	st->st_ino = pst.ino;
	st->st_nlink = pst.nlink;
	st->st_size = pst.size;
	st->st_mode = (pst.type == 1 /* T_DIR, include/stat.h */)
		? (S_IFDIR|0755) : (S_IFREG|0644);
	return 0;
}

int
fstat(int fd, struct stat *st)
{
	return __fstat(fd, st);
}

// __pthread_mutex_timedlock/__pthread_cond_timedwait/__timedwait:
// real musl's *actually blocking* contention paths
// (src/thread/pthread_mutex_timedlock.c, pthread_cond_timedwait.c,
// __timedwait.c) - unlike pthread_mutex_lock()/pthread_cond_
// broadcast()/pthread_rwlock_{rd,wr,un,tryrd,trywr}lock() and
// __pthread_rwlock_timed{rd,wr}lock's own fast paths (all real, all
// linked in - see the Makefile; the timed rwlock functions' fast
// path is simply an unconditional call to pthread_rwlock_try{rd,wr}
// lock() before ever considering a real wait), these are only ever
// reached once a lock is already held by (or a condvar is being
// waited on by) another thread. poc-os has no threads at all, so
// every lock musl/ldso/dynlink.c takes is always uncontended and
// these are link-time-only requirements, never actually called at
// runtime - if one ever *is* called, an assumption here broke, so
// each traps rather than silently returning a wrong answer.
//
// (__pthread_rwlock_timedrdlock/__pthread_rwlock_timedwrlock
// themselves are NOT stubbed here, unlike an earlier version of this
// file assumed - they're reached on every single lock/unlock, not
// just under contention, so they're real files now - see the
// Makefile. Confirmed by this port's first genuinely dynamically-
// linked test binary, musl/test/dyntest.c, trapping into what was
// then a stub the moment musl/ldso/dynlink.c took its first
// pthread_rwlock_wrlock() during startup.)
#include <pthread.h>
#include <time.h>

int
__pthread_mutex_timedlock(pthread_mutex_t *restrict m, const struct timespec *restrict at)
{
	(void)m; (void)at;
	__builtin_trap();
}

int
__pthread_cond_timedwait(pthread_cond_t *restrict c, pthread_mutex_t *restrict m, const struct timespec *restrict ts)
{
	(void)c; (void)m; (void)ts;
	__builtin_trap();
}

int
__timedwait(volatile int *addr, int val, int clk, const void *at, int priv)
{
	(void)addr; (void)val; (void)clk; (void)at; (void)priv;
	__builtin_trap();
}

// pthread_cond_timedwait (public name too - src/thread/
// pthread_cond_wait.c, already linked in, calls this exact name, not
// the __-prefixed one) - same "uncontended/unreachable, trap if
// proven wrong" reasoning as the four functions above.
int
pthread_cond_timedwait(pthread_cond_t *restrict c, pthread_mutex_t *restrict m, const struct timespec *restrict ts)
{
	(void)c; (void)m; (void)ts;
	__builtin_trap();
}

// __private_cond_signal: replaces the same-named function in
// src/thread/pthread_cond_timedwait.c (not linked in - see the trap
// stubs above), which pthread_cond_broadcast() (real, linked in - see
// the Makefile) calls unconditionally for every non-"shared" condvar,
// waiters or not. Real __private_cond_signal walks the condvar's
// waiter list (c->_c_head/_c_tail) waking up to n of them; poc-os is
// single-threaded, so that list is always empty (nothing ever calls
// pthread_cond_wait() successfully to add itself to it - see the trap
// stub above), and real __private_cond_signal reduces to exactly this
// on an empty list: nothing to wake, return 0.
int
__private_cond_signal(pthread_cond_t *c, int n)
{
	(void)c; (void)n;
	return 0;
}
