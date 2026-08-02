/* Phase-4 smoke test: proves SYS_mmap/SYS_munmap (kernel/sysproc.c)
 * actually hand back usable, writable memory, and that munmap of the
 * top-of-address-space region it returned works - not a general mmap
 * (see the syscall's doc comment in include/syscall.h), just the
 * narrow anonymous-at-the-top case musl's allocator needs.
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

#define LEN 8192  /* 2 pages, so the write-then-reread spans a page boundary */

int
main(void)
{
	long addr;
	unsigned char *p;
	int i;

	/* addr, len, prot, flags, fd, offset - prot/flags are ignored by
	 * poc-os's sys_mmap, fd must be -1 (anonymous). */
	addr = __syscall6(__NR_mmap, 0, LEN, 0, 0, -1, 0);
	wrstr("mmap: addr="); wrhex((unsigned long)addr);
	if (addr < 0) {
		wrstr("mmap: mmap failed - FAIL\n");
		__syscall1(__NR_exit, 1);
	}

	p = (unsigned char *)addr;
	for (i = 0; i < LEN; i++)
		p[i] = (unsigned char)i;
	for (i = 0; i < LEN; i++) {
		if (p[i] != (unsigned char)i) {
			wrstr("mmap: readback mismatch - FAIL\n");
			__syscall1(__NR_exit, 1);
		}
	}
	wrstr("mmap: readback ok\n");

	if (__syscall2(__NR_munmap, addr, LEN) < 0) {
		wrstr("mmap: munmap failed - FAIL\n");
		__syscall1(__NR_exit, 1);
	}
	wrstr("mmap: PASS\n");
	__syscall1(__NR_exit, 0);
	return 0;
}
