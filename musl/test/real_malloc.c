/* Phase-6 smoke test: real musl malloc (oldmalloc backend - see
 * musl/tools notes / the Makefile's MUSL_MALLOC_OBJS) on top of the
 * real crt1/__libc_start_main chain proven by real_hello.c. Exercises
 * malloc/realloc/calloc/free and the new SYS_brk the oldmalloc backend
 * calls directly (kernel/sysproc.c's sys_brk()).
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
wrstr(const char *s)
{
	write(1, s, strlen(s));
}

int
main(void)
{
	char *p = malloc(64);
	if (!p) {
		wrstr("malloc: malloc(64) failed - FAIL\n");
		return 1;
	}
	memset(p, 'A', 64);

	char *q = realloc(p, 4096);
	if (!q) {
		wrstr("malloc: realloc(4096) failed - FAIL\n");
		return 1;
	}
	if (q[0] != 'A' || q[63] != 'A') {
		wrstr("malloc: realloc lost data - FAIL\n");
		return 1;
	}

	int *arr = calloc(100, sizeof(int));
	if (!arr) {
		wrstr("malloc: calloc failed - FAIL\n");
		return 1;
	}
	for (int i = 0; i < 100; i++)
		if (arr[i] != 0) {
			wrstr("malloc: calloc not zeroed - FAIL\n");
			return 1;
		}

	free(q);
	free(arr);

	wrstr("malloc: PASS\n");
	return 0;
}
