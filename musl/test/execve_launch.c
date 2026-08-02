/* Phase-2 smoke test, launcher half: run normally (native SYS_exec,
 * entry = main() like any other poc-os user program - see
 * musl/test/hello.c), it hand-builds an argv/envp array and issues the
 * new SYS_execve directly (raw syscall_arch.h, no libc) to relaunch
 * "execve_verify", the half that actually checks the stack layout
 * kernel/exec.c's execve() built. Two separate binaries, not one,
 * because a single ELF has exactly one entry point, chosen at link
 * time - it can't switch between "argc/argv in %rdi/%rsi" (how this
 * file is entered) and "argc/argv/envp/auxv on the stack" (how
 * execve_verify is entered) depending on which syscall launched it.
 */
#include "syscall_arch.h"
#include "bits/syscall.h.in"

static void
wrstr(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	__syscall3(__NR_write, 1, (long)s, n);
}

int
main(void)
{
	static char *newargv[] = { "execve_verify", "arg-one", "arg-two", 0 };
	static char *newenvp[] = { "MUSL_PORT_TEST=1", "SECOND=2", 0 };

	wrstr("execve_launch: calling SYS_execve\n");
	__syscall3(__NR_execve, (long)"execve_verify", (long)newargv, (long)newenvp);
	wrstr("execve_launch: SYS_execve returned - FAIL\n");
	__syscall1(__NR_exit, 1);
	return 0;
}
