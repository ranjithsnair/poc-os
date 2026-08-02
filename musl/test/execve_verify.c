/* Phase-2 smoke test, verify half: linked with entry point _start_asm
 * (not main - see musl/tools/build-execve-test.sh), the same way
 * musl's real crt1 (_start in musl/arch/x86_64/crt_arch.h) is entered:
 * %rsp points straight at argc, no argc/argv in registers. Decodes
 * that raw stack by hand and reports what it found, so a wrong stack
 * layout from kernel/exec.c's execve() shows up as wrong printed
 * values (or a fault) instead of silently working.
 */
#include "syscall_arch.h"
#include "bits/syscall.h.in"

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

/* Mirrors include/auxv.h's AT_PAGESZ/AT_ENTRY independently on
 * purpose, so this test still catches a mismatch if one of them ever
 * drifts from the other. */
#define TEST_AT_PAGESZ 6
#define TEST_AT_ENTRY  9

__asm__(
	".global _start_asm\n"
	"_start_asm:\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call _start_verify\n"
);

void
_start_verify(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;
	long *auxv;
	int i;
	long pagesz = -1, entry = -1;

	wrstr("execve_verify: decoding raw stack\n");

	wrstr("argc="); wrhex((unsigned long)argc);
	for (i = 0; i < argc; i++)
		wrstr(argv[i]);
	for (i = 0; envp[i]; i++)
		wrstr(envp[i]);
	/* auxv starts right after envp's own NULL terminator, i.e. after
	 * envc+1 entries - it can't be cast straight from envp the way
	 * argv->envp can, because unlike argc, envc isn't known until
	 * this loop has actually walked to envp's NULL. */
	auxv = (long *)(envp + i + 1);

	for (i = 0; auxv[i] || auxv[i+1]; i += 2) {
		if (auxv[i] == TEST_AT_PAGESZ)
			pagesz = auxv[i+1];
		if (auxv[i] == TEST_AT_ENTRY)
			entry = auxv[i+1];
	}
	wrstr("AT_PAGESZ="); wrhex((unsigned long)pagesz);
	wrstr("AT_ENTRY="); wrhex((unsigned long)entry);

	wrstr("execve_verify: done\n");
	__syscall1(__NR_exit, 0);
}
