/* compositor: GUI roadmap phase 8 - the real yutani-equivalent daemon.
 * Owns the framebuffer and mouse devices directly (same open/mknod-
 * fallback/ioctl/mmap sequence as fbtest.c/guitest.c before it), puts
 * the console into raw mode so it becomes the sole reader of keyboard
 * input (kernel/kbd.c only ever feeds one global console stream - see
 * bash/poc/rawtest.c for the termios pattern this reuses), and listens
 * on GUI_SOCK_PATH for client connections (gui/libgui/gui_proto.h's
 * bespoke protocol).
 *
 * A fixed MAXWIN-sized window list with a back-to-front z-order array
 * (same scope-cut scale as every prior phase), server-side decorations
 * (title bar + border drawn by the compositor itself via libgui.so's
 * rasterizer - not a separate decorator client, a deliberate
 * simplification while this is being bootstrapped), click-to-focus
 * with raise-to-front, and title-bar drag-to-move. One epoll_wait()
 * loop (kernel/epoll.c, GUI roadmap phase 3/6) over the listening
 * socket, every connected client socket, the mouse fd, and the raw
 * console fd - full-frame redraw after every event, the same
 * "simplicity over efficiency" call guitest.c and every phase since
 * has already made (tiny screen, tiny window count, no real-time
 * constraint).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include "gfx.h"
#include "gui_proto.h"
#include "wire.h"

extern long syscall(long, ...);

#define FRAMEBUFFER_MAJOR 2
#define MOUSE_MAJOR 3
#define FBIOGET_VSCREENINFO 1

struct fb_info {
	unsigned int xres, yres, pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

struct mousepkt {
	unsigned char buttons;
	signed char dx, dy;
};

#define MAXWIN 8
#define MAXEV (MAXWIN + 3)
#define TITLEBAR_H 20
#define BORDER 2
#define MOUSE_DRAIN_MAX 32 // cap on packets coalesced into one redraw - see
                            // its use in main()'s mfd branch

// Real cursor artwork: ToaruOS's own base/usr/share/cursor/normal.png,
// alpha-matted and cropped to its visible bounding box (17x23) via
// tools/genraw.py's "icon" mode (same RGBA raw pipeline as the desktop's
// terminal icon) - see gui/assets/images/cursor.raw. The crop was taken
// tight to the glyph's bbox, so the arrow's tip sits exactly at the raw
// image's own (0,0): drawing at (cursor_x, cursor_y) with no extra
// offset already puts the tip at the reported pointer position.
#define CURSOR_PATH "/usr/share/cursor.raw"
static struct gfx_image_rgba cursor_img; /* .pixels == 0 if the asset is missing */

// Fallback if cursor.raw didn't load: a crude 12x19 ASCII-art arrow,
// same top-left-is-the-tip hotspot convention as the real asset above.
// '#' black outline, '.' white fill, anything else (space, or the
// implicit '\0' padding on rows shorter than CURSOR_W) transparent.
// Fixed-size rows rather than `const char *` so a row that's too long
// is a compile error instead of an out-of-bounds read at draw time.
#define CURSOR_W 12
#define CURSOR_H 19
static const char cursor_bitmap[CURSOR_H][CURSOR_W + 1] = {
	"#",
	"##",
	"#.#",
	"#..#",
	"#...#",
	"#....#",
	"#.....#",
	"#......#",
	"#.......#",
	"#........#",
	"#.....#####",
	"#..#..#",
	"#.# #..#",
	"##   #..#",
	"#     #..#",
	"      #..#",
	"      #..#",
	"       ##",
	"",
};

static void
draw_cursor(struct gfx_surface *s, int x, int y)
{
	int row, col;

	if (cursor_img.pixels) {
		gfx_blit_alpha(s, x, y, &cursor_img, 0, 0, (int)cursor_img.w, (int)cursor_img.h);
		return;
	}

	for (row = 0; row < CURSOR_H; row++) {
		for (col = 0; col < CURSOR_W; col++) {
			char c = cursor_bitmap[row][col];
			int px, py;

			if (c != '#' && c != '.')
				continue;
			px = x + col;
			py = y + row;
			if (px < 0 || px >= (int)s->w || py < 0 || py >= (int)s->h)
				continue;
			*(unsigned int *)(s->pixels + (unsigned long)py * s->pitch + (unsigned long)px * 4) =
				gfx_pack(s, c == '#' ? 0x000000 : 0xFFFFFF);
		}
	}
}

// Flat-fill approximation of ToaruOS's default "Fancy" decoration theme
// (toaruos/lib/decor-fancy.c's ACTIVE_COLOR/INACTIVE_COLOR/
// BORDER_COLOR) - real hardware/theme sprite-sheets are out of scope
// here, but the same three colors carry the same look.
#define COLOR_BG        0x101820   // used only if the wallpaper asset is missing
#define COLOR_BORDER    0x3E3E3E
#define COLOR_TITLE_FOC 0xE2E2E2
#define COLOR_TITLE_UNF 0x939393
#define COLOR_TITLE_TXT 0x202020

#define WALLPAPER_PATH "/usr/share/wallpaper.raw"

struct window {
	int inuse;
	int fd;
	int surface_id;
	int shmfd;                /* kept open for the surface's lifetime -
	                            * see gui/libgui/client.c's own comment:
	                            * kernel/shm.c only refcounts open fds,
	                            * not active mmap() mappings, so closing
	                            * this early frees the pages while still
	                            * mapped. */
	struct gfx_surface surf; /* client's content area, mmap'd shm */
	int x, y;                /* top-left of the DECORATION (title bar) */
	int flags;                /* GUI_WIN_* from gui_proto.h */
	char title[GUI_TITLE_MAX];
	int committed;
};

static struct window windows[MAXWIN];
static int zorder[MAXWIN]; /* indices into windows[], back(0) to front(nz-1) */
static int nz;
static int focus_idx = -1;
static struct gfx_surface fbsurf;
// System-RAM shadow of fbsurf: redraw_all() composites into this, then
// flushes it to the real (mmap'd) framebuffer with one gfx_blit() at
// the end. Without it every gfx_fill_rect()/gfx_blit() call in
// redraw_all() lands on-screen individually - visibly flickering
// (wallpaper, then each window, then the cursor, drawn as separate
// frames) since nothing stops the display from scanning out a
// half-composited frame.
static struct gfx_surface backbuf;
static struct gfx_surface wallpaper; /* .pixels == 0 if the asset is missing */
static int cursor_x, cursor_y;
static int next_surface_id = 1;

static int
decor_w(struct window *w)
{
	if (w->flags & GUI_WIN_BORDERLESS)
		return (int)w->surf.w;
	return (int)w->surf.w + 2 * BORDER;
}

static int
decor_h(struct window *w)
{
	if (w->flags & GUI_WIN_BORDERLESS)
		return (int)w->surf.h;
	return (int)w->surf.h + TITLEBAR_H + BORDER;
}

static void
zorder_remove(int idx)
{
	int i, j;

	for (i = 0; i < nz; i++) {
		if (zorder[i] == idx) {
			for (j = i; j < nz - 1; j++)
				zorder[j] = zorder[j + 1];
			nz--;
			return;
		}
	}
}

static void
zorder_raise(int idx)
{
	zorder_remove(idx);
	zorder[nz++] = idx;
}

static void
set_focus(int idx)
{
	struct gui_msg_focus_event ev;

	if (focus_idx == idx)
		return;
	if (focus_idx >= 0 && windows[focus_idx].inuse) {
		ev.type = GUI_MSG_FOCUS_EVENT;
		ev.focused = 0;
		wire_send(windows[focus_idx].fd, &ev, sizeof(ev), -1);
	}
	focus_idx = idx;
	if (focus_idx >= 0) {
		ev.type = GUI_MSG_FOCUS_EVENT;
		ev.focused = 1;
		wire_send(windows[focus_idx].fd, &ev, sizeof(ev), -1);
	}
}

static void
remove_window(int epfd, int idx)
{
	struct window *w = &windows[idx];

	epoll_ctl(epfd, EPOLL_CTL_DEL, w->fd, 0);
	close(w->fd);
	close(w->shmfd);
	zorder_remove(idx);
	if (focus_idx == idx)
		focus_idx = nz > 0 ? zorder[nz - 1] : -1;
	w->inuse = 0;
}

static int
window_at(int px, int py, int *in_titlebar)
{
	int i, idx;
	struct window *w;

	for (i = nz - 1; i >= 0; i--) {
		idx = zorder[i];
		w = &windows[idx];
		if (px >= w->x && px < w->x + decor_w(w) &&
		    py >= w->y && py < w->y + decor_h(w)) {
			*in_titlebar = !(w->flags & GUI_WIN_BORDERLESS) &&
			               py < w->y + TITLEBAR_H;
			return idx;
		}
	}
	return -1;
}

static void
redraw_all(void)
{
	int i, idx;
	struct window *w;

	if (wallpaper.pixels)
		gfx_blit(&backbuf, 0, 0, &wallpaper, 0, 0, (int)wallpaper.w, (int)wallpaper.h);
	else
		gfx_fill_rect(&backbuf, 0, 0, (int)backbuf.w, (int)backbuf.h, COLOR_BG);

	for (i = 0; i < nz; i++) {
		idx = zorder[i];
		w = &windows[idx];
		if (w->flags & GUI_WIN_BORDERLESS) {
			if (w->committed)
				gfx_blit(&backbuf, w->x, w->y, &w->surf, 0, 0,
				         (int)w->surf.w, (int)w->surf.h);
			continue;
		}
		gfx_fill_rect(&backbuf, w->x, w->y, w->x + decor_w(w), w->y + decor_h(w), COLOR_BORDER);
		gfx_fill_rect(&backbuf, w->x, w->y, w->x + decor_w(w), w->y + TITLEBAR_H,
		              idx == focus_idx ? COLOR_TITLE_FOC : COLOR_TITLE_UNF);
		gfx_draw_string(&backbuf, w->x + 4, w->y + 4, w->title, COLOR_TITLE_TXT);
		if (w->committed)
			gfx_blit(&backbuf, w->x + BORDER, w->y + TITLEBAR_H,
			         &w->surf, 0, 0, (int)w->surf.w, (int)w->surf.h);
	}

	draw_cursor(&backbuf, cursor_x, cursor_y);

	gfx_blit(&fbsurf, 0, 0, &backbuf, 0, 0, (int)fbsurf.w, (int)fbsurf.h);
}

static void
handle_create_surface(int epfd, int fd, struct gui_msg_create_surface *req)
{
	int idx, shmfd;
	union gui_msg reply;
	static int cascade;

	for (idx = 0; idx < MAXWIN; idx++)
		if (!windows[idx].inuse)
			break;
	if (idx == MAXWIN || req->w <= 0 || req->h <= 0) {
		close(fd);
		return;
	}

	shmfd = syscall(SYS_shm_create, (unsigned int)(req->w * 4 * req->h));
	if (shmfd < 0) {
		close(fd);
		return;
	}

	memset(&reply, 0, sizeof(reply));
	reply.surface_created.type = GUI_MSG_SURFACE_CREATED;
	reply.surface_created.surface_id = next_surface_id++;
	reply.surface_created.w = (unsigned int)req->w;
	reply.surface_created.h = (unsigned int)req->h;
	reply.surface_created.pitch = (unsigned int)req->w * 4;
	reply.surface_created.bpp = 32;
	reply.surface_created.red_mask_size = fbsurf.red_mask_size;
	reply.surface_created.red_field_pos = fbsurf.red_field_pos;
	reply.surface_created.green_mask_size = fbsurf.green_mask_size;
	reply.surface_created.green_field_pos = fbsurf.green_field_pos;
	reply.surface_created.blue_mask_size = fbsurf.blue_mask_size;
	reply.surface_created.blue_field_pos = fbsurf.blue_field_pos;
	reply.surface_created.screen_w = fbsurf.w;
	reply.surface_created.screen_h = fbsurf.h;

	if (wire_send(fd, &reply, sizeof(reply), shmfd) != (int)sizeof(reply)) {
		close(shmfd);
		close(fd);
		return;
	}

	windows[idx].surf.pixels = mmap(0, (unsigned long)req->w * 4 * (unsigned long)req->h,
	                                 PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	if (windows[idx].surf.pixels == MAP_FAILED) {
		close(shmfd);
		close(fd);
		return;
	}
	windows[idx].inuse = 1;
	windows[idx].fd = fd;
	windows[idx].shmfd = shmfd;
	windows[idx].surface_id = reply.surface_created.surface_id;
	windows[idx].surf.w = (unsigned int)req->w;
	windows[idx].surf.h = (unsigned int)req->h;
	windows[idx].surf.pitch = (unsigned int)req->w * 4;
	windows[idx].surf.red_mask_size = fbsurf.red_mask_size;
	windows[idx].surf.red_field_pos = fbsurf.red_field_pos;
	windows[idx].surf.green_mask_size = fbsurf.green_mask_size;
	windows[idx].surf.green_field_pos = fbsurf.green_field_pos;
	windows[idx].surf.blue_mask_size = fbsurf.blue_mask_size;
	windows[idx].surf.blue_field_pos = fbsurf.blue_field_pos;
	memcpy(windows[idx].title, req->title, sizeof(windows[idx].title));
	windows[idx].title[GUI_TITLE_MAX - 1] = 0;
	windows[idx].committed = 0;
	windows[idx].flags = req->flags;
	if (req->x == -1 && req->y == -1) {
		windows[idx].x = 40 + 30 * cascade;
		windows[idx].y = 40 + 30 * cascade;
		cascade = (cascade + 1) % 6;
	} else {
		windows[idx].x = req->x;
		windows[idx].y = req->y;
	}

	zorder[nz++] = idx;
	// NO_FOCUS windows (desktop bar/icon) never steal keyboard focus -
	// they only care about pointer clicks, and the whole point is to
	// leave a real app window (e.g. the terminal) as the keyboard target.
	if (!(req->flags & GUI_WIN_NO_FOCUS))
		set_focus(idx);
}

int
main(void)
{
	int fbfd, mfd, lfd, epfd;
	struct fb_info fi;
	struct sockaddr_un addr;
	struct epoll_event ev, events[MAXEV];
	struct termios orig, raw;
	int dragging = -1, drag_off_x = 0, drag_off_y = 0;
	int i;

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("compositor: no usable framebuffer\n");
		return 1;
	}
	fbsurf.pixels = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbsurf.pixels == MAP_FAILED) {
		printf("compositor: framebuffer mmap failed\n");
		return 1;
	}
	fbsurf.w = fi.xres;
	fbsurf.h = fi.yres;
	fbsurf.pitch = fi.pitch;
	fbsurf.red_mask_size = fi.red_mask_size;
	fbsurf.red_field_pos = fi.red_field_pos;
	fbsurf.green_mask_size = fi.green_mask_size;
	fbsurf.green_field_pos = fi.green_field_pos;
	fbsurf.blue_mask_size = fi.blue_mask_size;
	fbsurf.blue_field_pos = fi.blue_field_pos;

	backbuf = fbsurf;
	backbuf.pitch = fbsurf.w * 4; /* tightly packed - gfx_blit() copies scanline-by-scanline, so a
	                               * different pitch than fbsurf's is fine */
	backbuf.pixels = malloc((unsigned long)backbuf.pitch * fbsurf.h);
	if (!backbuf.pixels) {
		printf("compositor: backbuf alloc failed\n");
		return 1;
	}

	mfd = open("mouse", O_RDONLY);
	if (mfd < 0) {
		syscall(SYS_mknod, "mouse", MOUSE_MAJOR, 0);
		mfd = open("mouse", O_RDONLY);
	}
	if (mfd < 0) {
		printf("compositor: no usable mouse\n");
		return 1;
	}

	if (tcgetattr(0, &orig) < 0) {
		printf("compositor: tcgetattr failed\n");
		return 1;
	}
	raw = orig;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &raw) < 0) {
		printf("compositor: tcsetattr(raw) failed\n");
		return 1;
	}

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("compositor: socket failed\n");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, GUI_SOCK_PATH);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 8) < 0) {
		printf("compositor: bind/listen failed\n");
		tcsetattr(0, TCSANOW, &orig);
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("compositor: epoll_create1 failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = lfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	ev.data.fd = mfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, mfd, &ev);
	ev.data.fd = 0;
	epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);

	cursor_x = (int)fbsurf.w / 2;
	cursor_y = (int)fbsurf.h / 2;
	// wallpaper.pixels stays 0 (redraw_all() falls back to COLOR_BG) if
	// the asset is missing or doesn't match the real resolution - no
	// general image scaler, see tools/genraw.py's own comment.
	gfx_load_raw(&wallpaper, WALLPAPER_PATH, &fbsurf);
	if (wallpaper.pixels && (wallpaper.w != fbsurf.w || wallpaper.h != fbsurf.h)) {
		free(wallpaper.pixels);
		wallpaper.pixels = 0;
	}
	// cursor_img.pixels stays 0 (draw_cursor() falls back to the ASCII
	// arrow) if the asset is missing.
	gfx_load_raw_rgba(&cursor_img, CURSOR_PATH);
	redraw_all();
	printf("compositor: listening on %s\n", GUI_SOCK_PATH);

	for (;;) {
		int n = epoll_wait(epfd, events, MAXEV, -1);

		if (n < 0)
			continue;

		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == lfd) {
				int cfd = accept(lfd, 0, 0);

				if (cfd >= 0) {
					ev.events = EPOLLIN;
					ev.data.fd = cfd;
					epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
				}
			} else if (fd == mfd) {
				// Drain mouse packets already queued (kernel/mouse.c's
				// MOUSEBUFSIZE-deep ring) before redrawing once, instead of
				// a full-frame redraw per packet - VirtualBox's PS/2 mouse
				// can enqueue a burst of these for one physical motion, and
				// without this a single mouse wiggle was doing dozens of
				// full-screen redraws. Peek for more with a zero-timeout
				// epoll_wait() rather than looping read() unconditionally:
				// this kernel has no O_NONBLOCK (kernel/mouse.c's
				// mouseread() genuinely sleeps when the ring is empty), so
				// an unconditional next read() would block the whole
				// compositor on the *next real* mouse move once drained.
				// If the peek instead reports some other fd ready, that fd
				// stays ready (epoll here is level-triggered) and gets
				// picked up by the outer epoll_wait() on its next spin, so
				// nothing is lost by stopping the drain here.
				//
				// MOUSE_DRAIN_MAX bounds this regardless: a real mouse under
				// continuous motion can keep the ring nonempty indefinitely
				// (the guest reading packets no faster than the host/VM
				// keeps enqueuing them), and an unbounded loop here would
				// starve every *other* fd - accept() on lfd, wire_recv() on
				// a client socket - for as long as the flood lasts. That
				// previously manifested as exactly this: touch the desktop
				// icon, the mouse motion needed to reach and click it kept
				// this loop spinning, so the newly launched terminal's
				// CREATE_SURFACE never got processed and the screen never
				// got redrawn (cursor looked frozen) until the mouse went
				// idle.
				int drained = 0;
				int wi = -1, pressed = 0, sent_pressed = -1; /* sent_pressed:
					-1 means "nothing sent yet this burst" */

				for (; drained < MOUSE_DRAIN_MAX; drained++) {
					struct mousepkt pkt;
					int in_title;
					struct epoll_event peek;

					if (read(mfd, &pkt, sizeof(pkt)) != (int)sizeof(pkt))
						break;
					cursor_x += pkt.dx;
					cursor_y -= pkt.dy;
					if (cursor_x < 0) cursor_x = 0;
					if (cursor_x >= (int)fbsurf.w) cursor_x = (int)fbsurf.w - 1;
					if (cursor_y < 0) cursor_y = 0;
					if (cursor_y >= (int)fbsurf.h) cursor_y = (int)fbsurf.h - 1;

					pressed = pkt.buttons & 0x1;
					wi = window_at(cursor_x, cursor_y, &in_title);
					if (dragging >= 0) {
						windows[dragging].x = cursor_x - drag_off_x;
						windows[dragging].y = cursor_y - drag_off_y;
						if (!pressed)
							dragging = -1;
					} else if (pressed) {
						if (wi >= 0) {
							zorder_raise(wi);
							// NO_FOCUS windows (desktop bar/icon) never take
							// keyboard focus - see handle_create_surface()'s own
							// comment. in_title is already forced 0 for
							// BORDERLESS windows (window_at() above), so a
							// borderless-but-focusable window (the login
							// screen) still can't be dragged (no titlebar to
							// grab) without needing a separate check here.
							if (!(windows[wi].flags & GUI_WIN_NO_FOCUS))
								set_focus(wi);
							if (in_title) {
								dragging = wi;
								drag_off_x = cursor_x - windows[wi].x;
								drag_off_y = cursor_y - windows[wi].y;
							}
						}
					}

					// Send a pointer event immediately on every button-state
					// transition (a client's click detection is edge-
					// triggered on consecutive events - see gui/desktop.c's
					// icon_was_pressed - so a press-then-release that both
					// land inside one drained burst must still produce two
					// separate messages, not just the final state), but
					// otherwise skip sends for a burst's interior packets;
					// one send after the loop below covers the settled
					// position. A client's whole incoming queue is only
					// SOCKQLEN (3, include/socket.h) messages deep - sending
					// per-packet here (up to MOUSE_DRAIN_MAX) reliably
					// overran that and wedged the compositor's wire_send()
					// on a client (gui/desktop.c) that was itself briefly
					// not draining its socket (blocked in launch_terminal()'s
					// waitpid()), which looked like a permanent hang: this
					// keeps traffic to roughly the same one-ish message per
					// burst the pre-coalescing code sent per packet.
					if (wi >= 0 && pressed != sent_pressed) {
						struct window *w = &windows[wi];
						struct gui_msg_pointer_event pev;
						int cx_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : BORDER;
						int cy_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : TITLEBAR_H;

						pev.type = GUI_MSG_POINTER_EVENT;
						pev.x = cursor_x - (w->x + cx_off);
						pev.y = cursor_y - (w->y + cy_off);
						pev.buttons = pressed;
						wire_send(w->fd, &pev, sizeof(pev), -1);
						sent_pressed = pressed;
					}

					if (epoll_wait(epfd, &peek, 1, 0) != 1 || peek.data.fd != mfd)
						break;
				}

				// Final settled position/button-state, unconditionally (not
				// gated on a transition): a burst that ends mid-drag or
				// mid-hover with no further button change would otherwise
				// never tell the client where the pointer actually stopped.
				if (wi >= 0) {
					struct window *w = &windows[wi];
					struct gui_msg_pointer_event pev;
					int cx_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : BORDER;
					int cy_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : TITLEBAR_H;

					pev.type = GUI_MSG_POINTER_EVENT;
					pev.x = cursor_x - (w->x + cx_off);
					pev.y = cursor_y - (w->y + cy_off);
					pev.buttons = pressed;
					wire_send(w->fd, &pev, sizeof(pev), -1);
				}
				redraw_all();
			} else if (fd == 0) {
				unsigned char c;

				if (read(0, &c, 1) != 1)
					continue;
				if (focus_idx >= 0) {
					struct gui_msg_key_event kev;

					kev.type = GUI_MSG_KEY_EVENT;
					kev.ch = c;
					wire_send(windows[focus_idx].fd, &kev, sizeof(kev), -1);
				}
			} else {
				union gui_msg msg;
				int r = wire_recv(fd, &msg, sizeof(msg), 0);
				int wi;

				if (r <= 0) {
					for (wi = 0; wi < MAXWIN; wi++)
						if (windows[wi].inuse && windows[wi].fd == fd) {
							remove_window(epfd, wi);
							redraw_all();
							break;
						}
					continue;
				}

				switch (msg.type) {
				case GUI_MSG_CREATE_SURFACE:
					handle_create_surface(epfd, fd, &msg.create_surface);
					redraw_all();
					break;
				case GUI_MSG_COMMIT:
					for (wi = 0; wi < MAXWIN; wi++)
						if (windows[wi].inuse && windows[wi].fd == fd)
							windows[wi].committed = 1;
					redraw_all();
					break;
				case GUI_MSG_DESTROY:
					for (wi = 0; wi < MAXWIN; wi++)
						if (windows[wi].inuse && windows[wi].fd == fd) {
							remove_window(epfd, wi);
							break;
						}
					redraw_all();
					break;
				default:
					break;
				}
			}
		}
	}
}
