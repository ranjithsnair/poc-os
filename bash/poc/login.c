/* login: prompts for a poc-os account (name + password, plaintext - see
 * /etc/passwd's own format), then permanently drops from dinit's root
 * identity to that account and execs its shell. Replaces dinit's old
 * direct execve("/usr/bin/bash", ...) - see dinit.c's own comment for
 * the boot-flow change. Same dynamic Scrt1.o+libc.so PIE build as
 * dinit.c/bash/su - see this file's own Makefile rule.
 *
 * Password entry disables terminal echo (via tcgetattr/tcsetattr - a
 * real, working path: kernel/sysproc.c's sys_ioctl() implements real
 * TCGETS/TCSETS, and kernel/console.c's consoleintr() honors the ECHO
 * bit while still processing backspace/line-editing under ICANON, which
 * stays set throughout - exactly like a real terminal's password
 * prompt).
 *
 * Never returns except by _exit() on an exec failure: a failed login
 * attempt loops back to the username prompt rather than exiting, so
 * dinit's fork+reap loop only ever has to relaunch this program if it
 * genuinely crashes, the same way it used to relaunch bash directly.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <pwd.h>

static int
readline_fd(int fd, char *buf, int cap)
{
	int i = 0;
	char c;

	while (i < cap - 1) {
		if (read(fd, &c, 1) != 1)
			break;
		if (c == '\n' || c == '\r')
			break;
		buf[i++] = c;
	}
	buf[i] = 0;
	return i;
}

static void
prompt(const char *s)
{
	write(1, s, strlen(s));
}

int
main(void)
{
	char name[64], pass[64];
	struct passwd *pw;
	struct termios orig, noecho;
	char shell_env[256], user_env[64], logname_env[64];
	char *sh_argv[3];
	char *sh_envp[6];

	for (;;) {
		prompt("poc-os login: ");
		if (readline_fd(0, name, sizeof(name)) <= 0)
			continue;

		prompt("Password: ");
		if (tcgetattr(0, &orig) == 0) {
			noecho = orig;
			noecho.c_lflag &= ~ECHO;
			tcsetattr(0, TCSANOW, &noecho);
		}
		readline_fd(0, pass, sizeof(pass));
		tcsetattr(0, TCSANOW, &orig);
		prompt("\n");

		pw = getpwnam(name);
		if (pw == NULL || strcmp(pw->pw_passwd, pass) != 0) {
			prompt("Login incorrect\n");
			continue;
		}

		/* Group before user: dropping the real/effective/saved uid
		 * away from root (kernel/sysproc.c's sys_setuid()) also
		 * drops the permission to change gid at all - see that
		 * function's own comment on POSIX setuid()/setgid() rules. */
		if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
			prompt("login: failed to switch identity\n");
			continue;
		}
		chdir(pw->pw_dir);

		snprintf(shell_env, sizeof(shell_env), "HOME=%s", pw->pw_dir);
		snprintf(user_env, sizeof(user_env), "USER=%s", pw->pw_name);
		snprintf(logname_env, sizeof(logname_env), "LOGNAME=%s", pw->pw_name);

		sh_argv[0] = "-bash";
		sh_argv[1] = "-i";
		sh_argv[2] = 0;
		sh_envp[0] = "PATH=/usr/bin";
		sh_envp[1] = shell_env;
		sh_envp[2] = "TERM=dumb";
		sh_envp[3] = user_env;
		sh_envp[4] = logname_env;
		sh_envp[5] = 0;

		execve(pw->pw_shell, sh_argv, sh_envp);
		prompt("login: exec shell failed\n");
		_exit(1);
	}
}
