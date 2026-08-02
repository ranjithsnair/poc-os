/* Phase-3 smoke test: proves SYS_arch_prctl(ARCH_SET_FS, ...) plus
 * kernel/trap.c's per-return WRMSR(MSR_FS_BASE, ...) actually work -
 * this is what every musl program needs even single-threaded, since
 * musl keeps errno (and its whole pthread struct) TLS-relative (see
 * musl/arch/x86_64/pthread_arch.h's __get_tp: `mov %fs:0,%tp`).
 *
 * Sets up a minimal "TCB": a static buffer whose first 8 bytes point
 * at itself (the standard x86-64 TLS convention musl relies on - the
 * thread pointer is "whatever %fs:0 reads back", and it conventionally
 * reads back its own address), installs it via arch_prctl, then reads
 * it back two ways: once via %fs:0 (proving the MSR actually took
 * effect for real user-mode memory access, not just that the syscall
 * returned 0), and once by re-reading the buffer directly to confirm
 * nothing about the earlier syscall corrupted it.
 */
#include "syscall_arch.h"
#include "bits/syscall.h.in"
#include "syscall.h"  /* poc-os's own, for ARCH_SET_FS - see -Iinclude in Makefile */

static void
wr(const char *s, long n)
{
	__syscall3(__NR_write, 1, (long)s, n);
}

static void
wrstr(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	wr(s, n);
}

static void
wrhex(unsigned long v)
{
	char buf[19];
	int i;
	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++) {
		int nib = (v >> ((15 - i) * 4)) & 0xf;
		buf[2 + i] = nib < 10 ? '0' + nib : 'a' + nib - 10;
	}
	buf[18] = '\n';
	wr(buf, sizeof(buf));
}

static unsigned long tcb[8];

int
main(void)
{
	unsigned long via_fs;

	tcb[0] = (unsigned long)&tcb[0];  /* self-pointer, musl's convention */

	wrstr("tls: &tcb="); wrhex((unsigned long)&tcb[0]);

	if (__syscall2(__NR_arch_prctl, ARCH_SET_FS, (long)&tcb[0]) != 0) {
		wrstr("tls: arch_prctl failed - FAIL\n");
		__syscall1(__NR_exit, 1);
	}

	__asm__ volatile ("mov %%fs:0, %0" : "=r"(via_fs));
	wrstr("tls: %fs:0  ="); wrhex(via_fs);
	wrstr("tls: tcb[0] ="); wrhex(tcb[0]);

	if (via_fs == (unsigned long)&tcb[0] && tcb[0] == (unsigned long)&tcb[0]) {
		wrstr("tls: PASS\n");
		__syscall1(__NR_exit, 0);
	}
	wrstr("tls: FAIL\n");
	__syscall1(__NR_exit, 1);
	return 0;
}
