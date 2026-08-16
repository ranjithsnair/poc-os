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
#define CURSOR_HALF 5

#define COLOR_BG        0x101820
#define COLOR_BORDER    0x606060
#define COLOR_TITLE_FOC 0x2040A0
#define COLOR_TITLE_UNF 0x404040
#define COLOR_TITLE_TXT 0xFFFFFF
#define COLOR_CURSOR    0xFFFFFF

struct window {
	int inuse;
	int fd;
	int surface_id;
	struct gfx_surface surf; /* client's content area, mmap'd shm */
	int x, y;                /* top-left of the DECORATION (title bar) */
	char title[GUI_TITLE_MAX];
	int committed;
};

static struct window windows[MAXWIN];
static int zorder[MAXWIN]; /* indices into windows[], back(0) to front(nz-1) */
static int nz;
static int focus_idx = -1;
static struct gfx_surface fbsurf;
static int cursor_x, cursor_y;
static int next_surface_id = 1;

static int
decor_w(struct window *w)
{
	return (int)w->surf.w + 2 * BORDER;
}

static int
decor_h(struct window *w)
{
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
			*in_titlebar = (py < w->y + TITLEBAR_H);
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

	gfx_fill_rect(&fbsurf, 0, 0, (int)fbsurf.w, (int)fbsurf.h, COLOR_BG);

	for (i = 0; i < nz; i++) {
		idx = zorder[i];
		w = &windows[idx];
		gfx_fill_rect(&fbsurf, w->x, w->y, w->x + decor_w(w), w->y + decor_h(w), COLOR_BORDER);
		gfx_fill_rect(&fbsurf, w->x, w->y, w->x + decor_w(w), w->y + TITLEBAR_H,
		              idx == focus_idx ? COLOR_TITLE_FOC : COLOR_TITLE_UNF);
		gfx_draw_string(&fbsurf, w->x + 4, w->y + 4, w->title, COLOR_TITLE_TXT);
		if (w->committed)
			gfx_blit(&fbsurf, w->x + BORDER, w->y + TITLEBAR_H,
			         &w->surf, 0, 0, (int)w->surf.w, (int)w->surf.h);
	}

	gfx_fill_rect(&fbsurf, cursor_x - CURSOR_HALF, cursor_y - CURSOR_HALF,
	              cursor_x + CURSOR_HALF, cursor_y + CURSOR_HALF, COLOR_CURSOR);
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

	if (wire_send(fd, &reply, sizeof(reply), shmfd) != (int)sizeof(reply)) {
		close(shmfd);
		close(fd);
		return;
	}

	windows[idx].surf.pixels = mmap(0, (unsigned long)req->w * 4 * (unsigned long)req->h,
	                                 PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	close(shmfd);
	if (windows[idx].surf.pixels == MAP_FAILED) {
		close(fd);
		return;
	}
	windows[idx].inuse = 1;
	windows[idx].fd = fd;
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
	windows[idx].x = 40 + 30 * cascade;
	windows[idx].y = 40 + 30 * cascade;
	cascade = (cascade + 1) % 6;

	zorder[nz++] = idx;
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
				struct mousepkt pkt;
				int pressed, wi, in_title;

				if (read(mfd, &pkt, sizeof(pkt)) != (int)sizeof(pkt))
					continue;
				cursor_x += pkt.dx;
				cursor_y -= pkt.dy;
				if (cursor_x < 0) cursor_x = 0;
				if (cursor_x >= (int)fbsurf.w) cursor_x = (int)fbsurf.w - 1;
				if (cursor_y < 0) cursor_y = 0;
				if (cursor_y >= (int)fbsurf.h) cursor_y = (int)fbsurf.h - 1;

				pressed = pkt.buttons & 0x1;
				if (dragging >= 0) {
					windows[dragging].x = cursor_x - drag_off_x;
					windows[dragging].y = cursor_y - drag_off_y;
					if (!pressed)
						dragging = -1;
				} else if (pressed) {
					wi = window_at(cursor_x, cursor_y, &in_title);
					if (wi >= 0) {
						zorder_raise(wi);
						set_focus(wi);
						if (in_title) {
							dragging = wi;
							drag_off_x = cursor_x - windows[wi].x;
							drag_off_y = cursor_y - windows[wi].y;
						}
					}
				}

				if (focus_idx >= 0) {
					struct window *w = &windows[focus_idx];
					struct gui_msg_pointer_event pev;

					pev.type = GUI_MSG_POINTER_EVENT;
					pev.x = cursor_x - (w->x + BORDER);
					pev.y = cursor_y - (w->y + TITLEBAR_H);
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
