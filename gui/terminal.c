/* terminal: GUI roadmap phase 10 - a libgui.so client that forks
 * bash -i with stdin/stdout on anonymous pipes (bash/poc/
 * bashpipetest.c's already-verified pattern - TERM=dumb, isatty(0)
 * false, confirmed bash tolerates this) instead of a real pty, since
 * none exists in this kernel.
 *
 * The VT100/ANSI parsing and screen model come from vendored libvterm
 * (gui/libvterm/, MIT, dynamically linked as libvterm.so) rather than
 * this file's own escape parser - a real xterm-class state machine
 * (SGR, cursor addressing, scrolling regions, UTF-8, ...) instead of
 * the ~200-line bounded CSI/SGR parser it replaced. TERM stays "dumb"
 * - a deliberate, unchanged choice: forcing TERM=xterm without a real
 * PTY risks readline/curses programs probing capabilities this
 * pipe-based shell can't back up, so libvterm just ends up parsing
 * whatever escapes bash/coreutils *do* emit under non-tty detection
 * (colored `ls`, a colored prompt, `clear`) rather than this app
 * advertising full terminal capability.
 *
 * Forwards keystrokes received via the compositor's key_event messages
 * (only ever delivered while this window holds focus) into bash's
 * stdin pipe, translating '\r' to '\n' the way a real terminal's
 * canonical mode would, since bash expects a newline to process a line
 * with no pty of its own to do that translation for it. Also feeds
 * each keystroke straight into libvterm as if it were terminal output,
 * for the same reason the original parser's put_char() call did: with
 * no real pty, nothing else echoes a keystroke to the screen the way a
 * tty's line discipline normally would.
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
#include "vterm.h"

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

// Same 16-color ANSI palette the original parser used, now handed to
// libvterm's VTermState so its own SGR handling (30-37/90-97/40-47/
// 100-107, including the ones the old parser never implemented, like
// 38/48 extended color) resolves indexed colors the same way.
static const unsigned int palette[16] = {
	0x000000, 0xCC0000, 0x4E9A06, 0xC4A000,
	0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
	0x555753, 0xEF2929, 0x8AE234, 0xFCE94F,
	0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};
#define DEFAULT_FG 7
#define DEFAULT_BG 0

static VTerm *vt;
static VTermScreen *vscreen;
static int cell_w, cell_h;
// Current grid size - starts at the COLS/ROWS this window was created
// with, but changes on a GUI_EVENT_RESIZE (maximize/restore,
// gui/compositor.c's own GUI_MSG_RESIZE): vterm_set_size() reflows the
// real terminal state to match, so the shell continues at the new
// dimensions rather than just having its existing 80x24 grid stretched.
static int cols = COLS, rows = ROWS;

// Write end of the pipe feeding bash's stdin - libvterm calls this
// back for any bytes a parsed sequence needs to answer (e.g. a device
// attributes or cursor position query), the same fd keystrokes are
// forwarded to below.
static int pty_in_fd = -1;

// bash reads its own input a byte at a time with no line-editing of
// its own (grep confirms bash/input.c/parse.y never special-case 0x08/
// 0x7f) - on a real terminal that's fine, because the *tty driver's*
// canonical-mode line discipline is what erases a character on
// backspace before bash ever sees the byte. There's no real pty here
// (this file's own top comment), so nothing else does that job either:
// once a character has been write()'n to bash's stdin pipe, bash has
// already buffered it as part of the line it's building, and there is
// no byte terminal.c could send afterwards that would make bash
// un-see it. So backspace can only work if terminal.c holds the
// current line here instead of forwarding each character to bash
// immediately, exactly what a real tty's canonical mode would
// otherwise be doing - only the completed line (up to and including
// the terminating '\n') ever actually reaches bash's pipe.
#define LINE_BUF_MAX 4096
static char line_buf[LINE_BUF_MAX];
static int line_len;

static void
term_output(const char *s, size_t len, void *user)
{
	(void)user;
	if (pty_in_fd >= 0)
		write(pty_in_fd, s, len);
}

// No real pty backs this shell (see this file's own top comment), so
// nothing ever does the ONLCR translation a real tty's line discipline
// normally applies to outgoing bytes. bash (running under TERM=dumb)
// writes bare '\n' and trusts the terminal to return the cursor to
// column 0 itself - without this, libvterm (correctly, per real VT100
// semantics) only moves the cursor down a row on '\n' and leaves the
// column alone, so every line of output starts one column further
// right than the last.
static void
vterm_write_onlcr(VTerm *v, const char *buf, size_t n)
{
	char out[512];
	size_t oi = 0, i;

	for (i = 0; i < n; i++) {
		if (oi + 2 > sizeof(out)) {
			vterm_input_write(v, out, oi);
			oi = 0;
		}
		if (buf[i] == '\n')
			out[oi++] = '\r';
		out[oi++] = buf[i];
	}
	if (oi)
		vterm_input_write(v, out, oi);
}

static unsigned int
rgb_of(VTermColor col)
{
	vterm_screen_convert_color_to_rgb(vscreen, &col);
	return ((unsigned int)col.rgb.red << 16) |
	       ((unsigned int)col.rgb.green << 8) |
	       (unsigned int)col.rgb.blue;
}

static void
render(struct gui_conn *c, struct ttf_font *mono, struct ttf_font *mono_bold)
{
	VTermState *state = vterm_obtain_state(vt);
	VTermPos cursorpos;
	VTermPos pos;

	vterm_state_get_cursorpos(state, &cursorpos);

	for (pos.row = 0; pos.row < rows; pos.row++) {
		for (pos.col = 0; pos.col < cols; pos.col++) {
			VTermScreenCell cell;
			unsigned int fg, bg;
			uint32_t ch;
			char s[2];

			vterm_screen_get_cell(vscreen, pos, &cell);
			fg = rgb_of(cell.fg);
			bg = rgb_of(cell.bg);
			if (cell.attrs.reverse) {
				unsigned int t = fg;
				fg = bg;
				bg = t;
			}

			gfx_fill_rect(&c->surface, pos.col * cell_w, pos.row * cell_h,
			              (pos.col + 1) * cell_w, (pos.row + 1) * cell_h, bg);

			ch = cell.chars[0];
			if (ch == 0)
				ch = ' ';
			else if (ch > 0xFF)
				ch = '?'; // ttf_draw_string only takes single-byte codepoints
			s[0] = (char)ch;
			s[1] = 0;
			if (s[0] != ' ')
				ttf_draw_string(&c->surface, (cell.attrs.bold && mono_bold) ? mono_bold : mono,
				                 pos.col * cell_w, pos.row * cell_h, s, FONT_SIZE, fg);
		}
	}

	gfx_fill_rect(&c->surface, cursorpos.col * cell_w, (cursorpos.row + 1) * cell_h - 3,
	              (cursorpos.col + 1) * cell_w, (cursorpos.row + 1) * cell_h, COLOR_CURSOR);

	gui_commit(c);
}

int
main(void)
{
	struct gui_conn c;
	struct ttf_font *mono, *mono_bold;
	VTermState *state;
	VTermColor col;
	int in[2], out[2];
	pid_t pid;
	int epfd;
	struct epoll_event ev, events[2];
	char *sh_argv[3];
	char *sh_envp[3];
	int i;

	mono = ttf_load("/usr/share/fonts/dejavu/DejaVuSansMono.ttf");
	if (!mono) {
		printf("terminal: font load failed\n");
		return 1;
	}
	mono_bold = ttf_load("/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf");
	cell_w = ttf_string_width(mono, "M", FONT_SIZE);
	cell_h = ttf_line_height(mono, FONT_SIZE) + 4;
	if (cell_w <= 0) cell_w = 9;
	if (cell_h <= 4) cell_h = 18;

	vt = vterm_new(ROWS, COLS);
	vterm_set_utf8(vt, 1);
	vterm_output_set_callback(vt, term_output, NULL);
	vscreen = vterm_obtain_screen(vt);
	vterm_screen_reset(vscreen, 1);

	state = vterm_obtain_state(vt);
	vterm_color_rgb(&col, (palette[DEFAULT_FG] >> 16) & 0xff,
	                (palette[DEFAULT_FG] >> 8) & 0xff, palette[DEFAULT_FG] & 0xff);
	{
		VTermColor bg;

		vterm_color_rgb(&bg, (palette[DEFAULT_BG] >> 16) & 0xff,
		                (palette[DEFAULT_BG] >> 8) & 0xff, palette[DEFAULT_BG] & 0xff);
		vterm_state_set_default_colors(state, &col, &bg);
	}
	for (i = 0; i < 16; i++) {
		vterm_color_rgb(&col, (palette[i] >> 16) & 0xff, (palette[i] >> 8) & 0xff, palette[i] & 0xff);
		vterm_state_set_palette_color(state, i, &col);
	}

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
	render(&c, mono, mono_bold);

	if (pipe(in) < 0 || pipe(out) < 0) {
		printf("terminal: pipe failed\n");
		return 1;
	}
	pty_in_fd = in[1];

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
		int ei;

		if (n < 0)
			continue;

		for (ei = 0; ei < n; ei++) {
			if (events[ei].data.fd == out[0]) {
				char buf[256];
				int rn = read(out[0], buf, sizeof(buf));

				if (rn <= 0) {
					int status;

					waitpid(pid, &status, 0);
					gui_destroy(&c);
					return 0;
				}
				vterm_write_onlcr(vt, buf, (size_t)rn);
				render(&c, mono, mono_bold);
			} else if (events[ei].data.fd == c.fd) {
				struct gui_event gev;

				if (gui_recv_event(&c, &gev) < 0) {
					write(in[1], "exit\n", 5);
					waitpid(pid, 0, 0);
					return 0;
				}
				if (gev.type == GUI_EVENT_KEY) {
					int ch = gev.key.ch;

					if (ch == '\r')
						ch = '\n';

					if (ch == 0x7f || ch == 0x08) {
						// Backspace/DEL: only meaningful if there's
						// something in the current line to take back -
						// pop it locally (see line_buf's own comment)
						// and erase it on screen (move left, blank the
						// cell, move left again) rather than forwarding
						// anything to bash, which was never told about
						// this character in the first place.
						if (line_len > 0) {
							line_len--;
							vterm_write_onlcr(vt, "\b \b", 3);
							render(&c, mono, mono_bold);
						}
					} else if (ch == '\n') {
						if (line_len > 0)
							write(in[1], line_buf, (size_t)line_len);
						write(in[1], "\n", 1);
						line_len = 0;
						// No real pty backs this shell (see this file's
						// own top comment), so nothing else echoes a
						// keystroke to the screen the way a tty's line
						// discipline normally would - without this,
						// typed characters were sent to bash but never
						// appeared until its own output (e.g. the next
						// prompt) happened to redraw the grid.
						vterm_write_onlcr(vt, "\n", 1);
						render(&c, mono, mono_bold);
					} else if (ch >= 0x20 && ch < 0x7f) {
						char cc = (char)ch;

						if (line_len < LINE_BUF_MAX)
							line_buf[line_len++] = cc;
						vterm_write_onlcr(vt, &cc, 1);
						render(&c, mono, mono_bold);
					}
					// Anything else (the KEY_LF/KEY_RT/... special
					// codes kernel/kbd.h defines for arrow keys etc.,
					// all >0x7f) is silently ignored, same as before
					// this change - no mid-line cursor movement.
				} else if (gev.type == GUI_EVENT_RESIZE) {
					// c.surface is already remapped at the new pixel size
					// (gui_recv_event() itself did the mmap/munmap dance) -
					// just work out how many whole cells that is and
					// reflow libvterm to match, same as a real terminal
					// emulator resizing its pty. Any leftover fractional
					// row/col of pixels (the new size need not be an exact
					// multiple of cell_w/cell_h) is simply left blank by
					// render(), not stretched.
					int new_cols = (int)c.surface.w / cell_w;
					int new_rows = (int)c.surface.h / cell_h;

					if (new_cols < 1) new_cols = 1;
					if (new_rows < 1) new_rows = 1;
					vterm_set_size(vt, new_rows, new_cols);
					cols = new_cols;
					rows = new_rows;
					render(&c, mono, mono_bold);
				}
			}
		}
	}
}
