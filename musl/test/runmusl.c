/* Generic launcher for musl-linked binaries (entry = _start, expecting
 * the Linux-style argc/argv/envp/auxv stack kernel/exec.c's execve()
 * builds - see musl/test/execve_launch.c for why this can't just be
 * poc-os's native SYS_exec). Run as "runmusl <path> [args...]" from
 * the shell (native SYS_exec, so this file's own main() gets ordinary
 * argc/argv); re-execs <path> via the raw SYS_execve, forwarding
 * [args...] as its argv and a minimal fixed envp.
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
main(int argc, char **argv)
{
	static char *envp[] = { "HOME=/", "PATH=/", 0 };

	if (argc < 2) {
		wrstr("runmusl: usage: runmusl <path> [args...]\n");
		__syscall1(__NR_exit, 1);
	}

	__syscall3(__NR_execve, (long)argv[1], (long)(argv + 1), (long)envp);
	wrstr("runmusl: execve returned - FAIL\n");
	__syscall1(__NR_exit, 1);
	return 0;
}
