/* bashpipetest: throwaway diagnostic validating GUI roadmap phase 6's
 * one real unverified risk (see the plan doc at
 * /Users/ranjith/.claude/plans/structured-stargazing-pixel.md) - bash
 * has only ever run on poc-os attached to the real console device (a
 * real, if simple, tty). This forks bash -i (bash/poc/login.c's own
 * exact argv/envp shape) with stdin/stdout as plain anonymous pipes
 * instead - no pty layer exists in this kernel, so isatty(0) will
 * report false for bash, unlike every prior interactive session.
 * Writes "echo hello\n" to bash's stdin pipe and confirms "hello"
 * comes back on its stdout pipe within a bounded read loop. This has
 * to pass before gui/wm.c is built around the assumption that it does.
 *
 * dup2() (used to wire the pipe ends onto fds 0/1/2) is a real,
 * already-working shim (bash/poc/bash_shims.c, part of libc.so per the
 * Makefile's MUSL_LDSO_OBJS) built entirely on poc-os's own SYS_dup -
 * poc-os has no SYS_dup2/SYS_dup3 at all. Same dynamic Scrt1.o+libc.so
 * PIE build as every other bash/poc/*test.c before it.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int
main(void)
{
	int in[2], out[2];
	pid_t pid;
	char buf[256];
	int total, i, status;
	char *sh_argv[3];
	char *sh_envp[3];

	if (pipe(in) < 0 || pipe(out) < 0) {
		printf("bashpipetest: pipe failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("bashpipetest: fork failed\n");
		return 1;
	}
	if (pid == 0) {
		close(in[1]);
		close(out[0]);
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(out[1], 2);
		close(in[0]);
		close(out[1]);

		sh_argv[0] = "-bash";
		sh_argv[1] = "-i";
		sh_argv[2] = 0;
		sh_envp[0] = "PATH=/usr/bin";
		sh_envp[1] = "TERM=dumb";
		sh_envp[2] = 0;
		execve("/usr/bin/bash", sh_argv, sh_envp);
		_exit(1);
	}

	close(in[0]);
	close(out[1]);

	if (write(in[1], "echo hello\n", 11) != 11) {
		printf("bashpipetest: write to bash stdin failed\n");
		return 1;
	}

	total = 0;
	buf[0] = 0;
	for (i = 0; i < 50 && total < (int)sizeof(buf) - 1; i++) {
		int r = read(out[0], buf + total, sizeof(buf) - 1 - total);

		if (r > 0) {
			total += r;
			buf[total] = 0;
			if (strstr(buf, "hello"))
				break;
		} else if (r < 0) {
			break;
		}
	}

	printf("bashpipetest: got %d bytes from bash: [%s]\n", total, buf);

	write(in[1], "exit\n", 5);
	waitpid(pid, &status, 0);

	if (strstr(buf, "hello")) {
		printf("bashpipetest: PASS\n");
		return 0;
	}
	printf("bashpipetest: FAIL - bash-over-pipes did not echo back expected output\n");
	return 1;
}
