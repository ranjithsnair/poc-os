/* curses.c: poc-os's own minimal curses implementation - see curses.h's
 * own comment for what this covers and why (the exact subset GNU nano
 * 9.2 calls when built --enable-tiny) and what it deliberately doesn't
 * try to be (no terminfo, no real ncurses compatibility, no
 * damage-based diffing).
 *
 * Output model: every window (including stdscr) owns a plain
 * rows*cols character+reverse-video buffer in window-local
 * coordinates. wnoutrefresh() blits a window's buffer into stdscr's
 * (at the window's absolute begy/begx offset - stdscr itself is never
 * drawn to directly, since nano always uses its own newwin()s).
 * doupdate() then repaints the *entire* stdscr buffer to the real
 * screen on every call: one CUP per row plus one SGR each time the
 * reverse-video state changes within that row. Real curses instead
 * diffs against what's physically on screen and only redraws changed
 * cells; skipping that here is a deliberate simplification (this
 * console's CGA port I/O is trivially fast at 80x25) rather than a
 * correctness requirement - see the Stage 2 plan this came from.
 *
 * Input model: wgetch() reads single raw bytes from fd 0. Stage 1's
 * kernel raw-mode work (kernel/console.c) already delivers keystrokes
 * one at a time with no kernel echo, and kernel/kbd.c already decodes
 * arrows/Home/End/PgUp/PgDn/Ins/Del into the single private-use bytes
 * 0xE0-0xE9 (include/kbd.h's KEY_HOME..KEY_DEL) - so unlike a real
 * terminal, there is no multi-byte ANSI input escape sequence to
 * parse here, just a table translating those bytes into whatever this
 * curses.h picked for KEY_UP etc.
 */
#include <curses.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

struct _win {
	int rows, cols;
	int begy, begx;		/* absolute position on stdscr; 0 for stdscr */
	int cury, curx;		/* next-write position, window-local */
	int attrs;		/* currently active A_* bitmask (wattron/wattroff) */
	int keypad_on;
	unsigned char *ch;	/* rows*cols, window-local [row*cols+col] */
	unsigned char *rev;	/* rows*cols, 1 = reverse video, else 0 */
};

WINDOW *stdscr;
WINDOW *curscr;
int COLS, LINES;

static struct termios saved_termios;
static int ended = 1;
static WINDOW *cursor_win;	/* last window wnoutrefresh()'d - doupdate()
				 * parks the real cursor at its position */
static int pending = -1;	/* ungetch()'s one-slot pushback */

/* poc-os's own kbd.c (include/kbd.h) special-key byte codes, translated
 * to this header's KEY_* values when a window has keypad(win, TRUE).
 */
#define POC_KEY_HOME 0xE0
#define POC_KEY_END  0xE1
#define POC_KEY_UP   0xE2
#define POC_KEY_DN   0xE3
#define POC_KEY_LF   0xE4
#define POC_KEY_RT   0xE5
#define POC_KEY_PGUP 0xE6
#define POC_KEY_PGDN 0xE7
#define POC_KEY_INS  0xE8
#define POC_KEY_DEL  0xE9

static WINDOW *
mkwindow(int rows, int cols, int begy, int begx)
{
	WINDOW *w = malloc(sizeof(*w));

	if (!w)
		return NULL;
	w->rows = rows;
	w->cols = cols;
	w->begy = begy;
	w->begx = begx;
	w->cury = w->curx = 0;
	w->attrs = A_NORMAL;
	w->keypad_on = 0;
	w->ch = calloc((size_t)rows * cols, 1);
	w->rev = calloc((size_t)rows * cols, 1);
	if (!w->ch || !w->rev) {
		free(w->ch);
		free(w->rev);
		free(w);
		return NULL;
	}
	memset(w->ch, ' ', (size_t)rows * cols);
	return w;
}

WINDOW *
initscr(void)
{
	struct termios raw;
	struct winsize ws;

	tcgetattr(0, &saved_termios);
	raw = saved_termios;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);

	if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		LINES = ws.ws_row;
		COLS = ws.ws_col;
	} else {
		LINES = 25;
		COLS = 80;
	}

	stdscr = mkwindow(LINES, COLS, 0, 0);
	/* curscr is never drawn to - wrefresh() special-cases it (see its
	 * own comment) - a 1x1 placeholder is enough to give it a real,
	 * distinct, non-NULL identity. */
	curscr = mkwindow(1, 1, 0, 0);
	cursor_win = NULL;
	pending = -1;
	ended = 0;

	/* Real ANSI clear + home, so the first doupdate() starts from a
	 * known blank screen rather than whatever cooked-mode output
	 * (e.g. the shell prompt that launched this program) left behind.
	 */
	write(1, "\x1b[2J\x1b[1;1H", 10);

	return stdscr;
}

int
endwin(void)
{
	char buf[24];
	int len;

	len = snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[0m", LINES);
	write(1, buf, len);
	tcsetattr(0, TCSANOW, &saved_termios);
	ended = 1;
	return OK;
}

int
isendwin(void)
{
	return ended;
}

WINDOW *
newwin(int nlines, int ncols, int begin_y, int begin_x)
{
	return mkwindow(nlines, ncols, begin_y, begin_x);
}

int
delwin(WINDOW *win)
{
	if (win == stdscr)
		return OK;
	if (win == cursor_win)
		cursor_win = NULL;
	free(win->ch);
	free(win->rev);
	free(win);
	return OK;
}

/* raw()/noecho()/nonl(): initscr() already put fd 0 into single-
 * character, no-echo raw mode unconditionally (see above) - nano
 * calls these right after initscr() (its usual sequence), by which
 * point there's nothing further to change, so these just accept and
 * report success. */
int
raw(void)
{
	return OK;
}

int
noecho(void)
{
	return OK;
}

int
nonl(void)
{
	return OK;
}

int
keypad(WINDOW *win, int bf)
{
	win->keypad_on = bf;
	return OK;
}

/* nodelay(): a real non-blocking switch, via tcsetattr(VMIN=0) - nano's
 * own read_it_in() (winio.c) depends on this actually working: it does
 * nodelay(TRUE) then loops wgetch() until ERR to slurp up any already-
 * buffered keystrokes (escape-sequence/paste-burst assembly), which
 * hangs forever on the first keystroke if wgetch() can't ever return
 * ERR for "nothing available right now" - a blocking-only nodelay() is
 * not a usable simplification here, unlike curs_set()/napms() above.
 * kernel/console.c's consoleread() honors VMIN==0 for exactly this
 * (Stage 1's termios plumbing, closed off here rather than earlier
 * since nothing needed it until nano did). Global, like every other
 * termios setting on this single-console OS - the window argument is
 * accepted only for API-compatibility, same as raw()/noecho()/etc. */
int
nodelay(WINDOW *win, int bf)
{
	struct termios t;

	(void)win;
	tcgetattr(0, &t);
	t.c_cc[VMIN] = bf ? 0 : 1;
	tcsetattr(0, TCSANOW, &t);
	return OK;
}

int
scrollok(WINDOW *win, int bf)
{
	(void)win; (void)bf;
	return OK;
}

/* curs_set(): accepted, ignored - hiding/showing the CGA hardware
 * cursor would need DECTCEM (ESC[?25l/h) support added to
 * kernel/console.c's escape parser, which isn't there yet. The cursor
 * stays visible throughout, a cosmetic-only gap. */
int
curs_set(int visibility)
{
	(void)visibility;
	return OK;
}

int
typeahead(int fd)
{
	(void)fd;
	return OK;
}

int
ungetch(int ch)
{
	pending = ch;
	return OK;
}

int
set_escdelay(int size)
{
	(void)size;
	return OK;
}

int
wgetch(WINDOW *win)
{
	unsigned char c;
	int n;

	if (pending != -1) {
		n = pending;
		pending = -1;
		return n;
	}

	/* n == 0 here means "no data available" (VMIN=0 non-blocking mode,
	 * set by nodelay(TRUE) above) - not "try again": in VMIN=1 mode
	 * (the default) the kernel never returns 0, it blocks until at
	 * least one byte exists, so this check alone correctly covers both
	 * modes without needing to distinguish them here. */
	n = read(0, &c, 1);
	if (n <= 0)
		return ERR;

	if (!win->keypad_on)
		return c;

	switch (c) {
	case POC_KEY_HOME: return KEY_HOME;
	case POC_KEY_END:  return KEY_END;
	case POC_KEY_UP:   return KEY_UP;
	case POC_KEY_DN:   return KEY_DOWN;
	case POC_KEY_LF:   return KEY_LEFT;
	case POC_KEY_RT:   return KEY_RIGHT;
	case POC_KEY_PGUP: return KEY_PPAGE;
	case POC_KEY_PGDN: return KEY_NPAGE;
	case POC_KEY_INS:  return KEY_IC;
	case POC_KEY_DEL:  return KEY_DC;
	default:           return c;
	}
}

int
waddch(WINDOW *win, unsigned int ch)
{
	unsigned int c = ch & A_CHARMASK;
	int attrs = win->attrs | (ch & A_ATTRMASK);
	int idx;

	if (c == '\n') {
		/* Clear to end of line, then drop to the next one - the
		 * usual curses waddch('\n') behavior. */
		for (; win->curx < win->cols; win->curx++) {
			idx = win->cury * win->cols + win->curx;
			win->ch[idx] = ' ';
			win->rev[idx] = 0;
		}
		win->curx = 0;
		if (win->cury < win->rows - 1)
			win->cury++;
		return OK;
	}

	if (win->cury >= 0 && win->cury < win->rows &&
	    win->curx >= 0 && win->curx < win->cols) {
		idx = win->cury * win->cols + win->curx;
		win->ch[idx] = (unsigned char)c;
		win->rev[idx] = (attrs & A_REVERSE) ? 1 : 0;
		win->curx++;
	}
	return OK;
}

int
waddnstr(WINDOW *win, const char *str, int n)
{
	int i;

	for (i = 0; (n < 0 || i < n) && str[i]; i++)
		waddch(win, (unsigned char)str[i]);
	return OK;
}

int
waddstr(WINDOW *win, const char *str)
{
	return waddnstr(win, str, -1);
}

int
wmove(WINDOW *win, int y, int x)
{
	if (y < 0 || y >= win->rows || x < 0 || x >= win->cols)
		return ERR;
	win->cury = y;
	win->curx = x;
	return OK;
}

int
wattron(WINDOW *win, int attrs)
{
	win->attrs |= attrs;
	return OK;
}

int
wattroff(WINDOW *win, int attrs)
{
	win->attrs &= ~attrs;
	return OK;
}

int
wnoutrefresh(WINDOW *win)
{
	int r, c, sy, sx, idx, sidx;

	for (r = 0; r < win->rows; r++) {
		sy = win->begy + r;
		if (sy < 0 || sy >= stdscr->rows)
			continue;
		for (c = 0; c < win->cols; c++) {
			sx = win->begx + c;
			if (sx < 0 || sx >= stdscr->cols)
				continue;
			idx = r * win->cols + c;
			sidx = sy * stdscr->cols + sx;
			stdscr->ch[sidx] = win->ch[idx];
			stdscr->rev[sidx] = win->rev[idx];
		}
	}
	cursor_win = win;
	return OK;
}

int
doupdate(void)
{
	char buf[256];
	int len, row, col, idx, lastrev, r;

	for (row = 0; row < LINES; row++) {
		len = snprintf(buf, sizeof(buf), "\x1b[%d;1H", row + 1);
		write(1, buf, len);
		lastrev = -1;
		for (col = 0; col < COLS; col++) {
			idx = row * COLS + col;
			r = stdscr->rev[idx] ? 1 : 0;
			if (r != lastrev) {
				write(1, r ? "\x1b[7m" : "\x1b[0m", 4);
				lastrev = r;
			}
			write(1, (char *)&stdscr->ch[idx], 1);
		}
	}
	write(1, "\x1b[0m", 4);

	if (cursor_win) {
		len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
		    cursor_win->begy + cursor_win->cury + 1,
		    cursor_win->begx + cursor_win->curx + 1);
		write(1, buf, len);
	}
	return OK;
}

int
wrefresh(WINDOW *win)
{
	/* wrefresh(curscr): real curses' idiom for "redraw everything,
	 * unconditionally" - doupdate() below already redraws every cell
	 * on every call (no damage-diffing - see this file's own top
	 * comment), so there's nothing curscr's own (never-drawn-to)
	 * buffer needs to contribute; skip the blit that would otherwise
	 * overwrite stdscr with curscr's blank content. */
	if (win == curscr)
		return doupdate();
	wnoutrefresh(win);
	return doupdate();
}

int
wclrtoeol(WINDOW *win)
{
	int idx;

	for (; win->curx < win->cols; win->curx++) {
		idx = win->cury * win->cols + win->curx;
		win->ch[idx] = ' ';
		win->rev[idx] = 0;
	}
	return OK;
}

int
wscrl(WINDOW *win, int n)
{
	int rowbytes = win->cols;
	int i;

	if (n == 0)
		return OK;
	if (n > 0) {
		/* Scroll content up: row i takes row i+n's content. */
		if (n >= win->rows) {
			memset(win->ch, ' ', (size_t)win->rows * win->cols);
			memset(win->rev, 0, (size_t)win->rows * win->cols);
			return OK;
		}
		memmove(win->ch, win->ch + (size_t)n * rowbytes,
		    (size_t)(win->rows - n) * rowbytes);
		memmove(win->rev, win->rev + (size_t)n * rowbytes,
		    (size_t)(win->rows - n) * rowbytes);
		for (i = win->rows - n; i < win->rows; i++) {
			memset(win->ch + (size_t)i * rowbytes, ' ', rowbytes);
			memset(win->rev + (size_t)i * rowbytes, 0, rowbytes);
		}
	} else {
		n = -n;
		if (n >= win->rows) {
			memset(win->ch, ' ', (size_t)win->rows * win->cols);
			memset(win->rev, 0, (size_t)win->rows * win->cols);
			return OK;
		}
		memmove(win->ch + (size_t)n * rowbytes, win->ch,
		    (size_t)(win->rows - n) * rowbytes);
		memmove(win->rev + (size_t)n * rowbytes, win->rev,
		    (size_t)(win->rows - n) * rowbytes);
		for (i = 0; i < n; i++) {
			memset(win->ch + (size_t)i * rowbytes, ' ', rowbytes);
			memset(win->rev + (size_t)i * rowbytes, 0, rowbytes);
		}
	}
	return OK;
}

/* wredrawln(): real curses marks these physical lines as needing a
 * hardware redraw even if their cached content looks unchanged -
 * doupdate() here already redraws every line unconditionally on every
 * call, so there's nothing to mark. */
int
wredrawln(WINDOW *win, int beg_line, int num_lines)
{
	(void)win; (void)beg_line; (void)num_lines;
	return OK;
}

int
mvwaddch(WINDOW *win, int y, int x, unsigned int ch)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddch(win, ch);
}

int
mvwaddstr(WINDOW *win, int y, int x, const char *str)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddstr(win, str);
}

int
mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddnstr(win, str, n);
}

int
mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (wmove(win, y, x) == ERR)
		return ERR;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return waddstr(win, buf);
}

int
beep(void)
{
	char bel = '\a';

	write(1, &bel, 1);
	return OK;
}

/* napms(): a no-op - there's no working sleep/nanosleep syscall on
 * this OS yet (see include/syscall.h - only the tick-based, xv6-native
 * SYS_sleep exists, which musl's own usleep()/nanosleep() don't go
 * through). nano only uses this for brief cosmetic pauses (e.g.
 * flashing a message), so skipping the delay entirely doesn't affect
 * correctness. */
int
napms(int ms)
{
	(void)ms;
	return OK;
}

int
curses_dump(int fd)
{
	char buf[COLS > 0 ? COLS + 2 : 82];
	int row, col, idx;

	if (!stdscr)
		return ERR;
	for (row = 0; row < LINES; row++) {
		for (col = 0; col < COLS; col++) {
			idx = row * COLS + col;
			buf[col] = (stdscr->ch[idx] == ' ' && !stdscr->rev[idx])
			    ? '.' : (char)stdscr->ch[idx];
		}
		buf[COLS] = '\n';
		write(fd, buf, COLS + 1);
	}
	return OK;
}
