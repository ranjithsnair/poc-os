/* phase8test: throwaway launcher for GUI roadmap phase 8's real
 * compositor (gui/compositor.c) + two guiclient.c windows - fork()s
 * and exec()s all three directly rather than relying on bash's own
 * "&" background-job support, which has a known pre-existing gap
 * (bash/poc/bash_shims.c's dup2() tries to redirect a background
 * job's stdin from "/dev/null", which doesn't exist in this
 * filesystem - no /dev convention here at all, see fbtest.c's own
 * comment). Same "fork from one process, don't rely on the shell"
 * shape as bash/poc/ipctest.c and libguitest.c before it.
 *
 * The three children are deliberately not wait()'d for - they keep
 * running independently (reparented to init) after this process
 * exits, so bash's own prompt returns immediately and a screendump
 * can be taken (and synthetic QEMU-monitor mouse/keyboard events sent)
 * while they're still up.
 */
#include <stdio.h>
#include <unistd.h>

extern char **environ;

int
main(void)
{
	pid_t p;
	char *comp_argv[] = { "compositor", 0 };
	char *win1_argv[] = { "guiclient", "200", "150", "0000A0", "win1", 0 };
	char *win2_argv[] = { "guiclient", "200", "150", "A00000", "win2", 0 };

	p = fork();
	if (p == 0) {
		execve("/usr/bin/compositor", comp_argv, environ);
		printf("phase8test: exec compositor failed\n");
		_exit(1);
	}
	printf("phase8test: compositor pid %d\n", p);

	p = fork();
	if (p == 0) {
		execve("/usr/bin/guiclient", win1_argv, environ);
		printf("phase8test: exec guiclient(1) failed\n");
		_exit(1);
	}
	printf("phase8test: guiclient win1 pid %d\n", p);

	p = fork();
	if (p == 0) {
		execve("/usr/bin/guiclient", win2_argv, environ);
		printf("phase8test: exec guiclient(2) failed\n");
		_exit(1);
	}
	printf("phase8test: guiclient win2 pid %d\n", p);

	return 0;
}
