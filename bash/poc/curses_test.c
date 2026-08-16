/* curses_test: throwaway diagnostic for curses/curses.c (Stage 2 of the
 * nano port - see the plan) - proves the curses layer works end to end
 * (windows, attributes, refresh, and 0xE0-0xE9 -> KEY_* translation)
 * before Stage 3 vendors nano itself. Same dynamic Scrt1.o+libc.so PIE
 * build as rawtest/dinit.
 *
 * Draws a title bar / edit area / status bar layout matching nano's
 * own set_up_windows() (see nano.c), then reads a handful of
 * keystrokes. Since curses owns fd 1 for real screen content once
 * initscr() runs, all diagnostic output goes to a separate log file
 * (/curses_test.log) instead of stdout - including a plain-text dump
 * of the rendered screen via curses_dump() - so it can be read back
 * with `cat` after this exits, without needing a real ANSI-rendering
 * terminal attached (this OS's only headless test harness captures
 * raw serial output, not a rendered screen).
 */
#include <curses.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void
logkey(int fd, int k)
{
	const char *name = "?";

	switch (k) {
	case KEY_UP:     name = "KEY_UP"; break;
	case KEY_DOWN:   name = "KEY_DOWN"; break;
	case KEY_LEFT:   name = "KEY_LEFT"; break;
	case KEY_RIGHT:  name = "KEY_RIGHT"; break;
	case KEY_HOME:   name = "KEY_HOME"; break;
	case KEY_END:    name = "KEY_END"; break;
	case KEY_NPAGE:  name = "KEY_NPAGE"; break;
	case KEY_PPAGE:  name = "KEY_PPAGE"; break;
	case KEY_IC:     name = "KEY_IC"; break;
	case KEY_DC:     name = "KEY_DC"; break;
	default:
		if (k >= 0x20 && k < 0x7f)
			name = "(printable)";
		break;
	}
	dprintf(fd, "key=0x%x %s%s%c\n", k, name,
	    (k >= 0x20 && k < 0x7f) ? " '" : "",
	    (k >= 0x20 && k < 0x7f) ? k : ' ');
}

int
main(void)
{
	int dfd, i, k;
	WINDOW *topwin, *midwin, *footwin;

	dfd = open("/curses_test.log", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (dfd < 0)
		return 1;

	initscr();
	raw();
	noecho();
	nonl();

	topwin = newwin(1, COLS, 0, 0);
	midwin = newwin(LINES - 4, COLS, 1, 0);
	footwin = newwin(3, COLS, LINES - 3, 0);
	keypad(midwin, TRUE);
	keypad(footwin, TRUE);

	wattron(topwin, A_REVERSE);
	waddstr(topwin, "poc-os curses test - title bar");
	wattroff(topwin, A_REVERSE);
	wnoutrefresh(topwin);

	wmove(midwin, 0, 0);
	waddstr(midwin, "Hello from midwin, line 1\n");
	waddstr(midwin, "Second line of the edit area.");
	wnoutrefresh(midwin);

	wattron(footwin, A_REVERSE);
	wmove(footwin, 0, 0);
	waddstr(footwin, "[ status bar - reverse video ]");
	wattroff(footwin, A_REVERSE);
	wmove(footwin, 1, 0);
	waddstr(footwin, "^G Help  ^X Exit");
	wnoutrefresh(footwin);

	doupdate();

	dprintf(dfd, "LINES=%d COLS=%d\n", LINES, COLS);
	dprintf(dfd, "---screen dump---\n");
	curses_dump(dfd);
	dprintf(dfd, "---keys (reading 5)---\n");

	for (i = 0; i < 5; i++) {
		k = wgetch(midwin);
		logkey(dfd, k);
	}

	endwin();
	close(dfd);

	printf("curses_test: done, see /curses_test.log\n");
	return 0;
}
