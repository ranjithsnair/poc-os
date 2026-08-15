/* dinit: the initial user-level program, dynamically linked against
 * libc.so (Scrt1.o + real musl, not the static xv6-native ULIB
 * user/init.c used) - poc-os's new default convention for userland
 * software (see the Makefile's own BASH_PIC_CFLAGS comment). Same
 * console-setup-then-fork-reap structure as user/init.c, just built
 * against a real hosted libc and starting bash instead of sh.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "syscall.h"

static char *const argv[] = { "-bash", "-i", 0 };
static char *const envp[] = { "PATH=/usr/bin", "HOME=/", "TERM=dumb", 0 };

int
main(void)
{
	pid_t pid, wpid;

	/* initcode64.asm execs "/usr/bin/init" with no open file
	 * descriptors at all, so this open() is guaranteed to become
	 * fd 0; the two dup()s below then give fds 1 and 2 (stdout/
	 * stderr) the same underlying console file - identical to
	 * user/init.c's own setup. */
	if (open("console", O_RDWR) < 0) {
		/* Not musl's real mknod() (POSIX (path, mode, dev) shape) -
		 * poc-os's actual SYS_mknod (kernel/sysfile.c's sys_mknod())
		 * takes (path, major, minor) as three plain ints directly,
		 * the same xv6-native shape user/init.c's own mknod(path,
		 * 1, 1) call already relies on; calling through musl's
		 * POSIX-shaped wrapper here would silently pass mode/dev
		 * values in the major/minor slots instead. */
		syscall(SYS_mknod, "console", 1, 1);
		open("console", O_RDWR);
	}
	dup(0);	/* stdout */
	dup(0);	/* stderr */

	for (;;) {
		printf("init: starting bash\n");
		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			printf("init: fork failed\n");
			fflush(stdout);
			_exit(1);
		}
		if (pid == 0) {
			execve("/usr/bin/bash", argv, envp);
			printf("init: exec bash failed\n");
			fflush(stdout);
			_exit(1);
		}
		/* Reap every exited child, not just bash: init inherits any
		 * orphaned process whose original parent has already
		 * exited, and as the root of the process tree it must
		 * wait() for them so they don't linger as zombies forever -
		 * same reasoning as user/init.c's own comment. */
		while ((wpid = waitpid(-1, 0, 0)) >= 0 && wpid != pid) {
			printf("zombie!\n");
			fflush(stdout);
		}
	}
}
