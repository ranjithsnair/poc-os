/*
 * The real, exported POSIX ioctl() symbol -- distinct from mlibc's own
 * internal Sysdeps<Ioctl> hook in sysdeps.cpp (a different, mlibc-private
 * dispatch used by termios.cpp/unistd.cpp for TIOCGPGRP/TIOCSPGRP/
 * TIOCGSID). mlibc doesn't provide this function at all under our
 * options (it only ships one under the disabled glibc option -- see
 * meson.build's doc comment), but readline's rltty.c/terminal.c call it
 * directly for terminal-size queries. This kernel has no PTY/window
 * concept, so TIOCGWINSZ always reports a fixed 80x24 rather than
 * plumbing through to a real ioctl(TCGETS/TCSETS) -- those go through
 * tcgetattr()/tcsetattr() instead, not this function.
 */
#include <stdarg.h>
#include <sys/ioctl.h>

extern "C" int ioctl(int fd, unsigned long request, ...) {
	(void)fd;
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (request == TIOCGWINSZ) {
		struct winsize *ws = (struct winsize *)arg;
		ws->ws_row = 24;
		ws->ws_col = 80;
		ws->ws_xpixel = 0;
		ws->ws_ypixel = 0;
		return 0;
	}
	if (request == TIOCSWINSZ) {
		return 0; /* nothing to apply -- window size is fixed */
	}
	return 0; /* unrecognized request: succeed as a no-op, same stance kernel/src/syscall.c takes */
}
