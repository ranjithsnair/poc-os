/* rawtest: a throwaway diagnostic for the new ioctl(TCGETS/TCSETS/
 * TIOCGWINSZ) and raw console-mode support (kernel/console.c,
 * kernel/sysproc.c's sys_ioctl()) - not a real feature, just proof
 * that the kernel groundwork the later curses/nano stages will build
 * on actually works end to end: single-keystroke delivery, no kernel
 * echo, and cooked mode (so bash's own prompt) restored correctly on
 * exit. Same dynamic Scrt1.o+libc.so PIE build as dinit.c/bash - see
 * this file's own Makefile rule.
 *
 * Flags are set by hand rather than via cfmakeraw() (musl/src/termios/
 * cfmakeraw.c, gated behind _GNU_SOURCE/_BSD_SOURCE - see musl/include/
 * termios.h) so this only touches the two c_lflag bits the kernel
 * actually interprets (ICANON, ECHO), rather than pulling in a wider
 * feature-test macro to clear bits (IGNBRK, OPOST, ...) that don't do
 * anything on this console yet.
 *
 * Usage: run interactively from bash (not as init). Prints the raw
 * byte value of every keystroke until 'q' is pressed, then restores
 * the original termios.
 */
#include <stdio.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(void)
{
	struct termios orig, raw;
	struct winsize ws;
	unsigned char c;

	if (tcgetattr(0, &orig) < 0) {
		printf("rawtest: tcgetattr failed\n");
		return 1;
	}
	printf("rawtest: original c_lflag=0x%x (ICANON=%d ECHO=%d)\n",
	    orig.c_lflag, !!(orig.c_lflag & ICANON), !!(orig.c_lflag & ECHO));

	raw = orig;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &raw) < 0) {
		printf("rawtest: tcsetattr(raw) failed\n");
		return 1;
	}

	printf("rawtest: raw mode on - type keys, 'q' to quit\n");
	fflush(stdout);
	for (;;) {
		if (read(0, &c, 1) != 1)
			continue;
		printf("got byte 0x%02x ('%c')\n", c,
		    (c >= 0x20 && c < 0x7f) ? c : '.');
		fflush(stdout);
		if (c == 'q')
			break;
	}

	if (tcsetattr(0, TCSANOW, &orig) < 0) {
		printf("rawtest: tcsetattr(restore) failed\n");
		return 1;
	}
	printf("rawtest: restored cooked mode\n");

	if (ioctl(0, TIOCGWINSZ, &ws) == 0)
		printf("rawtest: window size %dx%d\n", ws.ws_row, ws.ws_col);
	else
		printf("rawtest: TIOCGWINSZ failed\n");

	return 0;
}
