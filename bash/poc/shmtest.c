/* shmtest: throwaway diagnostic for POSIX-ish shared memory
 * (kernel/shm.c - GUI roadmap phase 3, wl_shm's own mechanism: a
 * client mmaps a region, fills it with pixels, and passes the *fd* to
 * the compositor, which mmaps the same fd and sees the same bytes).
 * Proves the mapping is genuinely shared physical memory, not a copy:
 * parent and child each independently mmap() the *same* shm_create()d
 * fd (inherited via fork()'s fd-table duplication, not its usual
 * per-page memory *copy* - see kernel/vm.c's copyuvm(), which would
 * otherwise snapshot any mapping made *before* fork() into a private
 * page, which is exactly why both sides mmap() after forking rather
 * than once beforehand). A pipe provides the happens-before ordering
 * (child waits for the parent's write) - mmap() itself doesn't need
 * any ordering, since the physical pages already exist from
 * shm_create() and neither side's mmap() call copies data. Same
 * dynamic Scrt1.o+libc.so PIE build as fbtest.c before it.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern long syscall(long, ...);

#define SHM_SIZE 4096
#define MARKER 0xC0FFEE42u

int
main(void)
{
	int shmfd, pfd[2];
	pid_t pid;

	shmfd = syscall(SYS_shm_create, SHM_SIZE);
	if (shmfd < 0) {
		printf("shmtest: shm_create failed\n");
		return 1;
	}
	if (pipe(pfd) < 0) {
		printf("shmtest: pipe failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("shmtest: fork failed\n");
		return 1;
	}

	if (pid == 0) {
		unsigned int *p;
		char sync;

		close(pfd[1]);
		read(pfd[0], &sync, 1);  /* wait for parent's write */

		p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
		if (p == MAP_FAILED) {
			printf("shmtest: child mmap failed\n");
			_exit(1);
		}
		if (*p == MARKER)
			printf("shmtest: PASS - child read back parent's marker\n");
		else
			printf("shmtest: FAIL - child saw 0x%x, expected 0x%x\n", *p, MARKER);
		_exit(0);
	}

	{
		unsigned int *p;
		int status;

		close(pfd[0]);
		p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
		if (p == MAP_FAILED) {
			printf("shmtest: parent mmap failed\n");
			return 1;
		}
		*p = MARKER;
		write(pfd[1], "x", 1);
		waitpid(pid, &status, 0);
	}

	return 0;
}
