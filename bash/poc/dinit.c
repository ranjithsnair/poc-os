/* dinit: the initial user-level program, dynamically linked against
 * libc.so (Scrt1.o + real musl, not the static xv6-native ULIB
 * user/init.c used) - poc-os's new default convention for userland
 * software (see the Makefile's own BASH_PIC_CFLAGS comment). Same
 * console-setup-then-fork-reap structure as user/init.c.
 *
 * Boots straight into the graphical session (GUI roadmap phases 6-10)
 * rather than the old text-console bash/poc/login.c: kernel/vbe.c
 * switches the real display into a linear-framebuffer graphics mode
 * unconditionally at boot (before any of this runs), which means the
 * text console (kernel/console.c's CGA-memory + serial-mirrored
 * output) is never actually scanned out to the screen from that point
 * on - only ever visible over the serial port, invisible in a real
 * QEMU/VirtualBox/hardware display. A user just running the OS and
 * looking at the screen saw nothing at all until something drew real
 * pixels to the framebuffer - this is that "something."
 *
 * gui/compositor.c is launched once, as a persistent daemon (owns the
 * framebuffer/mouse/console for the whole boot, same reasoning a real
 * display server runs as a long-lived service); gui/login_gui.c is
 * relaunched every time its own process exits - which, thanks to
 * execve() replacing the image but keeping the pid (login_gui.c's own
 * comment), only actually happens once whatever it successfully
 * exec()'d into (gui/terminal.c) itself exits, e.g. typing "exit" in
 * the terminal - giving a real "log out, back to the login screen"
 * loop, the same shape bash/poc/login.c's own relaunch-on-exit loop
 * already had for plain bash.
 *
 * bash/poc/login.c (the old text-console login) is still built and
 * installed at /usr/bin/login for manual/development use (e.g. this
 * project's own QEMU-serial-console-driven testing technique, which
 * predates and doesn't need the graphical session) - just no longer
 * what boots by default.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* musl's real prototype for this is gated behind _GNU_SOURCE/
 * _BSD_SOURCE (musl/include/unistd.h), which this build's
 * -D_XOPEN_SOURCE=700 doesn't define - declaring it directly avoids
 * pulling in a broader feature-test macro just for this one call. */
extern long syscall(long, ...);

static char *const splash_argv[] = { "bootsplash", 0 };
static char *const splash_envp[] = { "PATH=/usr/bin", "HOME=/", "TERM=dumb", 0 };
static char *const comp_argv[] = { "compositor", 0 };
static char *const comp_envp[] = { "PATH=/usr/bin", "HOME=/", "TERM=dumb", 0 };
static char *const login_argv[] = { "login_gui", 0 };
static char *const login_envp[] = { "PATH=/usr/bin", "HOME=/", "TERM=dumb", 0 };

int
main(void)
{
	pid_t comp_pid, pid, wpid;
	pid_t splash_pid;

	/* initcode64.asm execs "/usr/bin/init" with no open file
	 * descriptors at all, so this open() is guaranteed to become
	 * fd 0; the two dup()s below then give fds 1 and 2 (stdout/
	 * stderr) the same underlying console file - identical to
	 * user/init.c's own setup. Only used for dinit's own diagnostic
	 * printf()s below (serial-visible, same as ever) - compositor
	 * and login_gui open their own framebuffer/mouse/console fds. */
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

	/* Draws the splash directly to the real framebuffer and exits -
	 * waited for (not backgrounded) so it's fully done, and has
	 * released the framebuffer device, before the compositor below
	 * ever opens it. gui/compositor.c no longer paints anything of its
	 * own at startup (see its own comment), so whatever this leaves on
	 * screen stays there untouched until gui/login_gui.c's first
	 * gui_commit() - no intermediate blank/wallpaper-only frame. */
	printf("init: starting bootsplash\n");
	fflush(stdout);
	splash_pid = fork();
	if (splash_pid == 0) {
		execve("/usr/bin/bootsplash", splash_argv, splash_envp);
		printf("init: exec bootsplash failed\n");
		fflush(stdout);
		_exit(1);
	}
	if (splash_pid > 0)
		waitpid(splash_pid, 0, 0);

	printf("init: starting compositor\n");
	fflush(stdout);
	comp_pid = fork();
	if (comp_pid < 0) {
		printf("init: fork failed\n");
		fflush(stdout);
		_exit(1);
	}
	if (comp_pid == 0) {
		execve("/usr/bin/compositor", comp_argv, comp_envp);
		printf("init: exec compositor failed\n");
		fflush(stdout);
		_exit(1);
	}

	for (;;) {
		printf("init: starting login_gui\n");
		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			printf("init: fork failed\n");
			fflush(stdout);
			_exit(1);
		}
		if (pid == 0) {
			execve("/usr/bin/login_gui", login_argv, login_envp);
			printf("init: exec login_gui failed\n");
			fflush(stdout);
			_exit(1);
		}
		/* Reap every exited child, not just this session's own pid:
		 * init inherits any orphaned process whose original parent
		 * has already exited, and as the root of the process tree
		 * it must wait() for them so they don't linger as zombies
		 * forever - same reasoning as user/init.c's own comment.
		 * Loops back to relaunch login_gui once the session pid
		 * itself exits (see this file's own header comment); if
		 * compositor unexpectedly exits first there is currently no
		 * separate recovery path - out of scope for this pass. */
		while ((wpid = waitpid(-1, 0, 0)) >= 0 && wpid != pid) {
			printf("zombie!\n");
			fflush(stdout);
		}
	}
}
