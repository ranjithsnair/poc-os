/* terminal: GUI roadmap phase 10 - a libgui.so client that forks
 * bash -i with stdin/stdout on anonymous pipes (bash/poc/
 * bashpipetest.c's already-verified pattern - TERM=dumb, isatty(0)
 * false, confirmed bash tolerates this) instead of a real pty, since
 * none exists in this kernel.
 *
 * Restyled (ToaruOS-style GUI rewrite): antialiased DejaVu Sans Mono
 * text (gui/libgui/ttf.c) instead of the original 8x16 bitmap font,
 * a real (if bounded) CSI/SGR escape parser driving a 16-color ANSI
 * palette per cell instead of one fixed fg/bg pair, and a drawn cursor
 * caret. TERM stays "dumb" - a deliberate, unchanged choice: forcing
 * TERM=xterm without a real PTY risks readline/curses programs probing
 * capabilities this pipe-based shell can't back up, so the parser just
 * handles whatever escapes bash/coreutils *do* emit under non-tty
 * detection (colored `ls`, a colored prompt, `clear`) rather than
 * advertising full terminal capability. See this file's own COLS/ROWS
 * comment for why the grid grew from the original 32x15 to 80x24.
 *
 * Forwards keystrokes received via the compositor's key_event messages
 * (only ever delivered while this window holds focus) into bash's
 * stdin pipe, translating '\r' to '\n' the way a real terminal's
 * canonical mode would, since bash expects a newline to process a line
 * with no pty of its own to do that translation for it.
 *
 * Multiplexes two fds - the compositor connection (for key/focus
 * events) and bash's stdout pipe (for output to render) - via
 * epoll_wait(), the same real per-fd readiness kernel/epoll.c has
 * provided since GUI roadmap phase 6 restored it (FD_PIPE readiness
 * predates that even, from phase 3).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/wait.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"
#include "ttf.h"

// 80x24 is the classic default terminal size. Real cell pixel
// dimensions are measured from the loaded DejaVu Sans Mono font at
// startup (main(), before gui_create_surface()), not assumed at
// compile time - but even a generous estimate (say 10x20px/cell)
// comes to 80*24*10*20*4 = 1,536,000 bytes, comfortably under
// SHM_MAXPAGES's 512-page/2MB cap (include/shm.h) once that cap was
// raised for exactly this (real antialiased glyphs need bigger cells
// than the original 8x16 bitmap font did, in the same pixel budget the
// old 32x15 grid used to just barely fit under the old 256KB cap).
#define COLS 80
#define ROWS 24
#define FONT_SIZE 15

#define COLOR_CURSOR 0xD3D7CF

static const unsigned int palette[16] = {
	0x000000, 0xCC0000, 0x4E9A06, 0xC4A000,
	0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
	0x555753, 0xEF2929, 0x8AE234, 0xFCE94F,
	0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};
#define DEFAULT_FG 7
#define DEFAULT_BG 0

struct cell {
	char ch;
	unsigned char fg, bg;
};

static struct cell grid[ROWS][COLS];
static int cur_row, cur_col;
static unsigned char cur_fg = DEFAULT_FG, cur_bg = DEFAULT_BG;

// Minimal CSI/SGR escape parser state, byte-at-a-time (output arrives
// in arbitrary-sized read() chunks, so this has to persist across
// calls rather than parse one buffer in isolation).
enum { ESC_NONE, ESC_SAW_ESC, ESC_CSI };
static int esc_state = ESC_NONE;
static int csi_params[8];
static int csi_nparam;
static int csi_cur_has_digit;

static int cell_w, cell_h;

static void
clear_cell(struct cell *c)
{
	c->ch = ' ';
	c->fg = DEFAULT_FG;
	c->bg = DEFAULT_BG;
}

static void
clear_row(int r)
{
	int col;

	for (col = 0; col < COLS; col++)
		clear_cell(&grid[r][col]);
}

static void
scroll_up(void)
{
	memmove(grid[0], grid[1], (ROWS - 1) * COLS * sizeof(struct cell));
	clear_row(ROWS - 1);
}

static void
advance_line(void)
{
	cur_col = 0;
	if (++cur_row >= ROWS) {
		scroll_up();
		cur_row = ROWS - 1;
	}
}

static void
put_visible(char ch)
{
	grid[cur_row][cur_col].ch = ch;
	grid[cur_row][cur_col].fg = cur_fg;
	grid[cur_row][cur_col].bg = cur_bg;
	if (++cur_col >= COLS)
		advance_line();
}

// Applies one SGR (CSI ... 'm') parameter - 0 resets, 30-37/90-97 set
// fg, 40-47/100-107 set bg, 39/49 reset fg/bg to default. Bold (1) is
// approximated as "bright" (the +8 palette half) applied to whatever
// color comes next/is already set, the common terminal convention.
static int sgr_bold;

static void
apply_sgr(int p)
{
	if (p == 0) {
		cur_fg = DEFAULT_FG;
		cur_bg = DEFAULT_BG;
		sgr_bold = 0;
	} else if (p == 1) {
		sgr_bold = 1;
	} else if (p == 22) {
		sgr_bold = 0;
	} else if (p >= 30 && p <= 37) {
		cur_fg = (unsigned char)((p - 30) + (sgr_bold ? 8 : 0));
	} else if (p == 39) {
		cur_fg = DEFAULT_FG;
	} else if (p >= 40 && p <= 47) {
		cur_bg = (unsigned char)(p - 40);
	} else if (p == 49) {
		cur_bg = DEFAULT_BG;
	} else if (p >= 90 && p <= 97) {
		cur_fg = (unsigned char)((p - 90) + 8);
	} else if (p >= 100 && p <= 107) {
		cur_bg = (unsigned char)((p - 100) + 8);
	}
}

static void
csi_final(char final)
{
	int i;

	if (csi_nparam == 0 && csi_cur_has_digit == 0)
		csi_nparam = 0; /* no params at all, e.g. plain ESC[H */

	switch (final) {
	case 'm':
		if (csi_nparam == 0)
			apply_sgr(0);
		for (i = 0; i < csi_nparam; i++)
			apply_sgr(csi_params[i]);
		break;
	case 'H':
	case 'f': {
		int row = csi_nparam > 0 ? csi_params[0] : 1;
		int col = csi_nparam > 1 ? csi_params[1] : 1;

		if (row < 1) row = 1;
		if (col < 1) col = 1;
		cur_row = row - 1 >= ROWS ? ROWS - 1 : row - 1;
		cur_col = col - 1 >= COLS ? COLS - 1 : col - 1;
		break;
	}
	case 'J': {
		int mode = csi_nparam > 0 ? csi_params[0] : 0;
		int r;

		if (mode == 2 || mode == 3) {
			for (r = 0; r < ROWS; r++)
				clear_row(r);
			cur_row = 0;
			cur_col = 0;
		}
		break;
	}
	case 'K': {
		int col;

		for (col = cur_col; col < COLS; col++)
			clear_cell(&grid[cur_row][col]);
		break;
	}
	case 'C': {
		int n = csi_nparam > 0 ? csi_params[0] : 1;

		cur_col += n;
		if (cur_col >= COLS)
			cur_col = COLS - 1;
		break;
	}
	case 'D': {
		int n = csi_nparam > 0 ? csi_params[0] : 1;

		cur_col -= n;
		if (cur_col < 0)
			cur_col = 0;
		break;
	}
	default:
		break; /* unrecognized final byte: sequence consumed, no-op */
	}
}

static void
put_char(char ch)
{
	if (esc_state == ESC_SAW_ESC) {
		if (ch == '[') {
			esc_state = ESC_CSI;
			csi_nparam = 0;
			csi_cur_has_digit = 0;
			memset(csi_params, 0, sizeof(csi_params));
		} else {
			esc_state = ESC_NONE; /* unsupported non-CSI escape: drop it */
		}
		return;
	}
	if (esc_state == ESC_CSI) {
		if (ch >= '0' && ch <= '9') {
			if (csi_nparam < (int)(sizeof(csi_params) / sizeof(csi_params[0]))) {
				csi_params[csi_nparam] = csi_params[csi_nparam] * 10 + (ch - '0');
				csi_cur_has_digit = 1;
			}
			return;
		}
		if (ch == ';') {
			if (csi_nparam < (int)(sizeof(csi_params) / sizeof(csi_params[0])) - 1)
				csi_nparam++;
			csi_cur_has_digit = 0;
			return;
		}
		if (ch >= 0x40 && ch <= 0x7E) {
			if (csi_cur_has_digit || csi_nparam > 0)
				csi_nparam++;
			csi_final(ch);
			esc_state = ESC_NONE;
			return;
		}
		return; /* intermediate byte (rare) - ignore, stay in CSI */
	}

	if (ch == 0x1b) {
		esc_state = ESC_SAW_ESC;
		return;
	}
	if (ch == '\n') {
		advance_line();
		return;
	}
	if (ch == '\r') {
		cur_col = 0;
		return;
	}
	if (ch == '\b' || ch == 0x7f) {
		if (cur_col > 0) {
			cur_col--;
			clear_cell(&grid[cur_row][cur_col]);
		}
		return;
	}
	if (ch == '\t') {
		int next = (cur_col / 8 + 1) * 8;

		while (cur_col < next && cur_col < COLS)
			put_visible(' ');
		return;
	}
	if ((unsigned char)ch < 0x20)
		return; /* other control bytes: drop, not rendered */

	put_visible(ch);
}

static void
render(struct gui_conn *c, struct ttf_font *mono)
{
	int r, col;

	for (r = 0; r < ROWS; r++) {
		for (col = 0; col < COLS; col++) {
			struct cell *cell = &grid[r][col];
			char s[2] = { cell->ch ? cell->ch : ' ', 0 };

			gfx_fill_rect(&c->surface, col * cell_w, r * cell_h,
			              (col + 1) * cell_w, (r + 1) * cell_h, palette[cell->bg]);
			if (s[0] != ' ')
				ttf_draw_string(&c->surface, mono, col * cell_w, r * cell_h,
				                 s, FONT_SIZE, palette[cell->fg]);
		}
	}

	gfx_fill_rect(&c->surface, cur_col * cell_w, (cur_row + 1) * cell_h - 3,
	              (cur_col + 1) * cell_w, (cur_row + 1) * cell_h, COLOR_CURSOR);

	gui_commit(c);
}

int
main(void)
{
	struct gui_conn c;
	struct ttf_font *mono;
	int in[2], out[2];
	pid_t pid;
	int epfd;
	struct epoll_event ev, events[2];
	char *sh_argv[3];
	char *sh_envp[3];
	int r;

	mono = ttf_load("/usr/share/fonts/dejavu/DejaVuSansMono.ttf");
	if (!mono) {
		printf("terminal: font load failed\n");
		return 1;
	}
	cell_w = ttf_string_width(mono, "M", FONT_SIZE);
	cell_h = ttf_line_height(mono, FONT_SIZE) + 4;
	if (cell_w <= 0) cell_w = 9;
	if (cell_h <= 4) cell_h = 18;

	for (r = 0; r < ROWS; r++)
		clear_row(r);

	{
		int tries = 0;

		while (gui_connect(&c, GUI_SOCK_PATH) < 0) {
			if (++tries > 100000) {
				printf("terminal: gui_connect failed\n");
				return 1;
			}
		}
	}
	if (gui_create_surface(&c, COLS * cell_w, ROWS * cell_h, -1, -1, 0, "terminal") < 0) {
		printf("terminal: gui_create_surface failed\n");
		return 1;
	}
	render(&c, mono);

	if (pipe(in) < 0 || pipe(out) < 0) {
		printf("terminal: pipe failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("terminal: fork failed\n");
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

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("terminal: epoll_create1 failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = c.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, c.fd, &ev);
	ev.data.fd = out[0];
	epoll_ctl(epfd, EPOLL_CTL_ADD, out[0], &ev);

	for (;;) {
		int n = epoll_wait(epfd, events, 2, -1);
		int i;

		if (n < 0)
			continue;

		for (i = 0; i < n; i++) {
			if (events[i].data.fd == out[0]) {
				char buf[256];
				int rn = read(out[0], buf, sizeof(buf));
				int j;

				if (rn <= 0) {
					int status;

					waitpid(pid, &status, 0);
					gui_destroy(&c);
					return 0;
				}
				for (j = 0; j < rn; j++)
					put_char(buf[j]);
				render(&c, mono);
			} else if (events[i].data.fd == c.fd) {
				struct gui_event gev;

				if (gui_recv_event(&c, &gev) < 0) {
					write(in[1], "exit\n", 5);
					waitpid(pid, 0, 0);
					return 0;
				}
				if (gev.type == GUI_EVENT_KEY) {
					char ch = (char)gev.key.ch;

					if (ch == '\r')
						ch = '\n';
					write(in[1], &ch, 1);

					// No real pty backs this shell (see this file's own
					// top comment), so nothing else echoes a keystroke to
					// the screen the way a tty's line discipline normally
					// would - without this, typed characters were sent to
					// bash but never appeared until its own output (e.g.
					// the next prompt) happened to redraw the grid.
					put_char(ch);
					render(&c, mono);
				}
			}
		}
	}
}
