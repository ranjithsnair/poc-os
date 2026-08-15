/* curses.h: poc-os's own minimal curses implementation - not a vendored
 * real ncurses/curses (there's no terminfo database or real termios
 * escape-input parsing here to build on; see curses/curses.c's own
 * comment) but a small, hand-written subset covering exactly the API
 * surface GNU nano 9.2 calls when built --enable-tiny (surveyed from
 * real nano source, not guessed - see the "Stage 2" plan this came
 * from). nano picks this header up via <curses.h> because its
 * definitions.h only reaches for <ncurses.h> when HAVE_NCURSES_H is
 * defined, which nano/poc/config.h (Stage 3) never defines.
 *
 * KEY_* values below are ours to choose freely - nano is compiled
 * against this header, not real ncurses, so there's no ABI to match.
 * They're kept above any single byte (0-255) so ordinary characters
 * and control codes can never collide with them.
 */
#ifndef _CURSES_H
#define _CURSES_H

/* Real ncurses.h pulls this in too (NCURSES_ENABLE_STDBOOL_H) - nano's
 * own headers assume <curses.h> already made `bool` available and
 * never include <stdbool.h> themselves. */
#include <stdbool.h>
#include <stdio.h>	/* real ncurses.h pulls this in too */
#include <stdarg.h>	/* likewise (transitively, via stdio.h in practice) */

#define TRUE  1
#define FALSE 0
#define ERR   (-1)
#define OK    (0)

/* Attributes: a bitmask, passed to wattron()/wattroff() and OR-able
 * into a waddch() character. Only NORMAL/REVERSE actually change what
 * reaches the screen (see curses.c) - BOLD/ITALIC are accepted and
 * kept per-cell but never change the CGA attribute byte, since the
 * console has no concept of either.
 */
#define A_NORMAL   0x00000000
#define A_REVERSE  0x00010000
#define A_BOLD     0x00020000
#define A_ITALIC   0x00040000
#define A_ATTRMASK 0x000f0000
#define A_CHARMASK 0x0000ffff

#define KEY_MIN        0x0100
#define KEY_BACKSPACE  0x0107
#define KEY_DC         0x014a
#define KEY_IC         0x014b
#define KEY_UP         0x0103
#define KEY_DOWN       0x0102
#define KEY_LEFT       0x0104
#define KEY_RIGHT      0x0105
#define KEY_HOME       0x0106
#define KEY_END        0x0168
#define KEY_NPAGE      0x0152
#define KEY_PPAGE      0x0153
#define KEY_ENTER      0x0157
#define KEY_BTAB       0x0161

/* Function keys: kernel/kbd.c has no scancode table entries for F1-F24
 * at all (include/kbd.h), so these can never actually be produced by
 * wgetch() - kept only so nano's own shortcut-table code (which binds
 * F-keys as alternates for things already reachable another way, e.g.
 * F1=Help is also ^G) compiles; the bindings are simply unreachable. */
#define KEY_F0         0x0200
#define KEY_F(n)       (KEY_F0 + (n))

/* Shifted/numeric-keypad-with-NumLock-off keys: same story as the
 * KEY_F block above - kernel/kbd.c has no scancode decoding that could
 * ever produce these (it only ever emits the unshifted KEY_HOME/
 * KEY_END/etc. from include/kbd.h), so these exist purely so nano's
 * own parse_kbinput() switch (which case-labels on them) compiles;
 * they're simply unreachable values wgetch() never returns. */
#define KEY_SLEFT      0x0171
#define KEY_SRIGHT     0x0172
#define KEY_A1         0x0173
#define KEY_A3         0x0174
#define KEY_B2         0x0175
#define KEY_C1         0x0176
#define KEY_C3         0x0177
#define KEY_SDC        0x0178
#define KEY_SCANCEL    0x0179
#define KEY_CANCEL     0x017a
#define KEY_SSUSPEND   0x017b
#define KEY_SUSPEND    0x017c
#define KEY_SBEG       0x017d
#define KEY_BEG        0x017e

typedef struct _win WINDOW;

extern WINDOW *stdscr;
extern WINDOW *curscr;	/* real curses' "what's physically on screen"
			 * placeholder - wrefresh(curscr) forces a full
			 * redraw; see curses.c's own comment. */
extern int COLS, LINES;

WINDOW *initscr(void);
int     endwin(void);
int     isendwin(void);
WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
int     delwin(WINDOW *win);

int     raw(void);
int     noecho(void);
int     nonl(void);
int     keypad(WINDOW *win, int bf);
int     nodelay(WINDOW *win, int bf);
int     scrollok(WINDOW *win, int bf);
int     curs_set(int visibility);

int     wgetch(WINDOW *win);
int     typeahead(int fd);
int     ungetch(int ch);
int     set_escdelay(int size);

int     waddch(WINDOW *win, unsigned int ch);
int     waddstr(WINDOW *win, const char *str);
int     waddnstr(WINDOW *win, const char *str, int n);
int     wmove(WINDOW *win, int y, int x);
int     wattron(WINDOW *win, int attrs);
int     wattroff(WINDOW *win, int attrs);
int     wnoutrefresh(WINDOW *win);
int     wrefresh(WINDOW *win);
int     doupdate(void);
int     beep(void);
int     napms(int ms);

int     wclrtoeol(WINDOW *win);
int     wscrl(WINDOW *win, int n);
int     wredrawln(WINDOW *win, int beg_line, int num_lines);
int     mvwaddch(WINDOW *win, int y, int x, unsigned int ch);
int     mvwaddstr(WINDOW *win, int y, int x, const char *str);
int     mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n);
int     mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);

/* Debug/introspection - poc-os-original addition, not part of any real
 * curses API. Dumps the current stdscr frame (what the last doupdate()
 * sent, or would send) as plain text to fd: one line per row, '.' for
 * a blank cell, attributes ignored. This OS's only headless test
 * harness captures raw serial output, not a rendered terminal, so a
 * test driver has no other way to "see" what's on screen - see
 * curses/curses.c's own comment.
 */
int     curses_dump(int fd);

#endif
