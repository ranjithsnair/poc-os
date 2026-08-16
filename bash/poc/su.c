/* su: switch to another poc-os account (default "root") from an
 * already-running shell - the setuid-on-exec counterpart to login.c
 * (see kernel/exec.c's su_uid/su_gid handling and mkfs/mkfs.c's
 * install_mode_override(), which installs this binary mode 4755 owned
 * by root). Verifies the target account's password while briefly
 * running as root (the setuid bit), then permanently drops to the
 * target account and execs its shell - same flow as login.c minus the
 * username prompt (taken from argv[1] instead, default "root") and
 * minus login's unconditional chdir (real su without "-" stays in the
 * caller's cwd).
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
main(int argc, char **argv)
{
	const char *target = argc > 1 ? argv[1] : "root";
	char pass[64];
	struct passwd *pw;
	struct termios orig, noecho;
	char shell_env[256], user_env[64], logname_env[64];
	char *sh_argv[3];
	char *sh_envp[6];

	pw = getpwnam(target);
	if (pw == NULL) {
		prompt("su: unknown user\n");
		return 1;
	}

	prompt("Password: ");
	if (tcgetattr(0, &orig) == 0) {
		noecho = orig;
		noecho.c_lflag &= ~ECHO;
		tcsetattr(0, TCSANOW, &noecho);
	}
	readline_fd(0, pass, sizeof(pass));
	tcsetattr(0, TCSANOW, &orig);
	prompt("\n");

	if (strcmp(pw->pw_passwd, pass) != 0) {
		prompt("su: Authentication failure\n");
		return 1;
	}

	/* Group before user - see login.c's identical comment. */
	if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
		prompt("su: failed to switch identity\n");
		return 1;
	}

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
	prompt("su: exec shell failed\n");
	return 1;
}
