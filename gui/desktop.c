/* desktop: GUI roadmap ToaruOS-style rewrite - the desktop shell
 * login_gui.c hands off to after a successful login (replacing the
 * old direct exec into a bare terminal). One process, two independent
 * compositor connections (the wire protocol has no notion of a client
 * owning more than one surface per connection - see gui_proto.h's own
 * comment - so two gui_connect()s, not one client with two surfaces,
 * is the simplest way for one process to own two windows):
 *
 *   - a full-width top bar (borderless, pinned at (0,0)) showing a
 *     live clock (kernel/lapic.c's cmostime(), wired up as the new
 *     SYS_date syscall purely for this)
 *   - a small desktop icon (borderless, fixed position) that launches
 *     /usr/bin/terminal (fork()+execve()) on click
 *
 * Both independently load the wallpaper raw asset and crop exactly
 * the region behind their own known screen position, the same "fake
 * transparency" trick login_gui.c uses for its glass box - the
 * compositor itself also draws the wallpaper as its native background
 * (gui/compositor.c), so this keeps both borderless windows visually
 * seamless against it with no real alpha compositing needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"
#include "ttf.h"

extern long syscall(long, ...);

#define SYS_date 69

// Mirrors include/date.h's struct rtcdate field-for-field - user-space
// has no access to poc-os's own include/ tree (see gui/compositor.c's
// own struct fb_info for the established precedent of a local
// ABI-compatible copy instead).
struct rtcdate {
	unsigned int second, minute, hour, day, month, year;
};

#define PANEL_H 27
#define ICON_W 72
#define ICON_H 72
#define ICON_X 20
#define ICON_Y 40
#define ICON_IMG_SIZE 48

#define COLOR_BAR_TINT   0x101418
#define BAR_ALPHA        170
#define COLOR_BAR_TEXT   0xFFFFFF
#define COLOR_ACCENT_TOP 0x5DA3EC
#define COLOR_ACCENT_BOT 0x3889DC
#define COLOR_ICON_LABEL 0xFFFFFF

struct desktop {
	struct gui_conn bar, icon;
	struct gfx_surface wallpaper_bar, wallpaper_icon;
	struct gfx_image_rgba icon_img;
	struct ttf_font *sans, *sans_bold;
};

// Double-fork: poc-os's wait() (kernel/proc.c) is a plain blocking
// xv6-style wait with no WNOHANG/waitpid, and desktop.c's event loop
// can't afford to block on it for however long a terminal session
// lasts. Forking twice and having the middle child exit immediately
// orphans the real terminal process straight to initproc (/usr/bin/
// init, bash/poc/dinit.c - see kernel/proc.c's exit()'s own "pass
// abandoned children to init" comment), which already reaps its
// children in a loop - so the terminal never becomes desktop.c's own
// zombie, and the one wait() call below only blocks for the instant
// it takes the middle child to fork-and-exit.
static void
launch_terminal(void)
{
	int pid = fork();

	if (pid == 0) {
		if (fork() == 0) {
			char *argv[] = { "terminal", 0 };
			char *envp[] = { 0 };

			execve("/usr/bin/terminal", argv, envp);
			_exit(1);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, 0, 0);
}

static void
format_clock(char *buf, unsigned long bufsz)
{
	struct rtcdate r;
	static const char *mname[] = {
		"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	if (syscall(SYS_date, &r) < 0 || r.month == 0 || r.month > 12) {
		buf[0] = 0;
		return;
	}
	snprintf(buf, bufsz, "%02u:%02u:%02u  %s %u, %u",
	         r.hour, r.minute, r.second, mname[r.month], r.day, r.year);
}

static void
render_bar(struct desktop *d)
{
	char clock[64];
	int w = (int)d->bar.surface.w;

	if (d->wallpaper_bar.pixels)
		gfx_blit(&d->bar.surface, 0, 0, &d->wallpaper_bar, 0, 0, w, PANEL_H);
	else
		gfx_fill_rect(&d->bar.surface, 0, 0, w, PANEL_H, 0x181818);
	gfx_fill_rect_alpha(&d->bar.surface, 0, 0, w, PANEL_H, COLOR_BAR_TINT, BAR_ALPHA);
	gfx_fill_rect_gradient(&d->bar.surface, 0, PANEL_H - 2, w, PANEL_H, COLOR_ACCENT_TOP, COLOR_ACCENT_BOT);

	ttf_draw_string(&d->bar.surface, d->sans_bold, 12, 6, "poc-os", 13, COLOR_BAR_TEXT);

	format_clock(clock, sizeof(clock));
	if (clock[0]) {
		int cw = ttf_string_width(d->sans, clock, 13);

		ttf_draw_string(&d->bar.surface, d->sans, w - cw - 14, 6, clock, 13, COLOR_BAR_TEXT);
	}

	gui_commit(&d->bar);
}

static void
render_icon(struct desktop *d, int hover)
{
	int img_x = (ICON_W - ICON_IMG_SIZE) / 2;
	const char *label = "Terminal";
	int label_w;

	if (d->wallpaper_icon.pixels)
		gfx_blit(&d->icon.surface, 0, 0, &d->wallpaper_icon, 0, 0, ICON_W, ICON_H);
	else
		gfx_fill_rect(&d->icon.surface, 0, 0, ICON_W, ICON_H, 0x181818);

	if (hover)
		gfx_fill_rounded_rect_alpha(&d->icon.surface, 2, 2, ICON_W - 2, ICON_H - 2, 10, 0xFFFFFF, 60);

	if (d->icon_img.pixels)
		gfx_blit_alpha(&d->icon.surface, img_x, 4, &d->icon_img, 0, 0, ICON_IMG_SIZE, ICON_IMG_SIZE);

	label_w = ttf_string_width(d->sans, label, 12);
	ttf_draw_string_shadow(&d->icon.surface, d->sans, (ICON_W - label_w) / 2, ICON_IMG_SIZE + 8,
	                        label, 12, COLOR_ICON_LABEL, 0x000000);

	gui_commit(&d->icon);
}

int
main(void)
{
	struct desktop d;
	struct epoll_event ev, events[2];
	int epfd;
	int icon_was_pressed = 0, icon_hover = 0, icon_hover_drawn = -1;

	memset(&d, 0, sizeof(d));

	if (gui_connect(&d.bar, GUI_SOCK_PATH) < 0 ||
	    gui_create_surface(&d.bar, 1, 1, -1, -1, GUI_WIN_BORDERLESS | GUI_WIN_NO_FOCUS, "desktop-bar") < 0) {
		printf("desktop: bar probe connect failed\n");
		return 1;
	}
	{
		int screen_w = (int)d.bar.screen_w;

		gui_destroy(&d.bar);
		if (gui_connect(&d.bar, GUI_SOCK_PATH) < 0 ||
		    gui_create_surface(&d.bar, screen_w, PANEL_H, 0, 0, GUI_WIN_BORDERLESS | GUI_WIN_NO_FOCUS, "desktop-bar") < 0) {
			printf("desktop: bar create failed\n");
			return 1;
		}
	}

	if (gui_connect(&d.icon, GUI_SOCK_PATH) < 0 ||
	    gui_create_surface(&d.icon, ICON_W, ICON_H, ICON_X, ICON_Y, GUI_WIN_BORDERLESS | GUI_WIN_NO_FOCUS, "desktop-icon") < 0) {
		printf("desktop: icon create failed\n");
		return 1;
	}

	d.sans = ttf_load("/usr/share/fonts/dejavu/DejaVuSans.ttf");
	d.sans_bold = ttf_load("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf");

	// Load the full wallpaper exactly once (both crops below read from
	// this same copy) and deliberately never free it: kernel/sysproc.c's
	// sys_munmap() only allows unmapping from the exact current top of
	// the process's address space (see its own comment), but musl's
	// free() for a large allocation can call munmap() on it regardless
	// of what's been allocated since - freeing this ~2.3MB buffer after
	// the two small crop mallocs below violates that ordering and
	// crashed (found via trace prints bisecting exactly this call).
	// One-time process-lifetime leak instead - harmless for a single
	// long-lived desktop shell process.
	{
		struct gfx_surface full;

		gfx_load_raw(&full, "/usr/share/wallpaper.raw", &d.bar.surface);
		if (full.pixels) {
			d.wallpaper_bar.w = d.bar.surface.w;
			d.wallpaper_bar.h = PANEL_H;
			d.wallpaper_bar.pitch = d.bar.surface.pitch;
			d.wallpaper_bar.pixels = malloc((unsigned long)d.bar.surface.pitch * PANEL_H);
			if (d.wallpaper_bar.pixels)
				gfx_blit(&d.wallpaper_bar, 0, 0, &full, 0, 0, (int)full.w, PANEL_H);

			d.wallpaper_icon.w = ICON_W;
			d.wallpaper_icon.h = ICON_H;
			d.wallpaper_icon.pitch = d.icon.surface.pitch;
			d.wallpaper_icon.pixels = malloc((unsigned long)d.icon.surface.pitch * ICON_H);
			if (d.wallpaper_icon.pixels)
				gfx_blit(&d.wallpaper_icon, 0, 0, &full, ICON_X, ICON_Y, ICON_W, ICON_H);
		}
	}
	gfx_load_raw_rgba(&d.icon_img, "/usr/share/icons/terminal.raw");

	render_bar(&d);
	render_icon(&d, 0);

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("desktop: epoll_create1 failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = d.bar.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, d.bar.fd, &ev);
	ev.data.fd = d.icon.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, d.icon.fd, &ev);

	for (;;) {
		int n = epoll_wait(epfd, events, 2, 1000);
		int i;

		if (n < 0)
			continue;
		if (n == 0) {
			render_bar(&d);   /* clock tick */
			continue;
		}
		for (i = 0; i < n; i++) {
			struct gui_event gev;
			struct gui_conn *src = events[i].data.fd == d.bar.fd ? &d.bar : &d.icon;

			if (gui_recv_event(src, &gev) < 0) {
				printf("desktop: connection closed\n");
				return 1;
			}
			if (gev.type != GUI_EVENT_POINTER || src != &d.icon)
				continue;

			icon_hover = gev.pointer.x >= 0 && gev.pointer.x < ICON_W &&
			             gev.pointer.y >= 0 && gev.pointer.y < ICON_H;
			if ((gev.pointer.buttons & 1) && !icon_was_pressed && icon_hover)
				launch_terminal();
			icon_was_pressed = (gev.pointer.buttons & 1) != 0;
			// Only the hover flag changes what render_icon() actually
			// draws - redrawing (and gui_commit()'s synchronous round
			// trip to the compositor) on every single pointer event,
			// including the many fired while the mouse just sits still
			// or moves around inside an already-hovered icon, made the
			// compositor's wire_send() to this window's socket back up
			// (kernel/socket.c's 3-message queue cap) and block, which
			// stalls its single-threaded event loop entirely - looked
			// like the whole system freezing (mouse included) while
			// the cursor was anywhere near the icon.
			if (icon_hover != icon_hover_drawn) {
				render_icon(&d, icon_hover);
				icon_hover_drawn = icon_hover;
			}
		}
	}
}
