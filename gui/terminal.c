/* terminal: GUI roadmap phase 10 - a libgui.so client that forks
 * bash -i with stdin/stdout on anonymous pipes (bash/poc/
 * bashpipetest.c's already-verified pattern - TERM=dumb, isatty(0)
 * false, confirmed bash tolerates this) instead of a real pty, since
 * none exists in this kernel. Renders bash's pipe output as a plain
 * text grid using libgui.so's font renderer: '\n' advances a row
 * (scrolling when the grid fills), '\r' returns to column 0, other
 * bytes place-and-wrap - no ANSI escape parsing, consistent with
 * TERM=dumb rarely emitting any. Forwards keystrokes received via the
 * compositor's key_event messages (only ever delivered while this
 * window holds focus) into bash's stdin pipe, translating '\r' to
 * '\n' the way a real terminal's canonical mode would, since bash
 * expects a newline to process a line with no pty of its own to do
 * that translation for it.
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

// COLS*ROWS is bounded by include/shm.h's SHM_MAXPAGES cap (64 pages
// = 256KB - "plenty for a" small compositor surface, per that file's
// own comment): at CELL_W*CELL_H*4 bytes/cell, COLS*ROWS must stay
// <= 512 cells. 32x15 (480 cells, 245760 bytes) leaves a small
// margin under the exact 262144-byte cap rather than sitting right on
// it. The established convention here (see GUI roadmap phase 4's own
// history) is to shrink the surface to fit, not raise the cap.
#define COLS 32
#define ROWS 15
#define CELL_W 8
#define CELL_H 16

#define COLOR_BG 0x000000
#define COLOR_FG 0x30D030

static char grid[ROWS][COLS];
static int cur_row, cur_col;

static void
scroll_up(void)
{
	memmove(grid[0], grid[1], (ROWS - 1) * COLS);
	memset(grid[ROWS - 1], 0, COLS);
}

static void
put_char(char ch)
{
	if (ch == '\n') {
		cur_col = 0;
		if (++cur_row >= ROWS) {
			scroll_up();
			cur_row = ROWS - 1;
		}
		return;
	}
	if (ch == '\r') {
		cur_col = 0;
		return;
	}
	grid[cur_row][cur_col] = ch;
	if (++cur_col >= COLS) {
		cur_col = 0;
		if (++cur_row >= ROWS) {
			scroll_up();
			cur_row = ROWS - 1;
		}
	}
}

static void
render(struct gui_conn *c)
{
	int r, col;

	gfx_fill_rect(&c->surface, 0, 0, COLS * CELL_W, ROWS * CELL_H, COLOR_BG);
	for (r = 0; r < ROWS; r++) {
		for (col = 0; col < COLS; col++) {
			char ch = grid[r][col];

			if (ch >= 32 && ch < 127)
				gfx_draw_glyph(&c->surface, col * CELL_W, r * CELL_H, ch, COLOR_FG);
		}
	}
	gui_commit(c);
}

int
main(void)
{
	struct gui_conn c;
	int in[2], out[2];
	pid_t pid;
	int epfd;
	struct epoll_event ev, events[2];
	char *sh_argv[3];
	char *sh_envp[3];

	{
		int tries = 0;

		while (gui_connect(&c, GUI_SOCK_PATH) < 0) {
			if (++tries > 100000) {
				printf("terminal: gui_connect failed\n");
				return 1;
			}
		}
	}
	if (gui_create_surface(&c, COLS * CELL_W, ROWS * CELL_H, "terminal") < 0) {
		printf("terminal: gui_create_surface failed\n");
		return 1;
	}
	render(&c);

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
				int r = read(out[0], buf, sizeof(buf));
				int j;

				if (r <= 0) {
					int status;

					waitpid(pid, &status, 0);
					gui_destroy(&c);
					return 0;
				}
				for (j = 0; j < r; j++)
					put_char(buf[j]);
				render(&c);
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
				}
			}
		}
	}
}
