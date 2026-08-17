/* login_gui: GUI roadmap phase 9 - a graphical login screen, launched
 * as the compositor's first client (gui/compositor.c starts as root
 * at boot, before any user session exists). Reuses the *existing*
 * authentication logic from bash/poc/login.c (getpwnam() against the
 * real /etc/passwd, phase 1's permcheck infra) verbatim - only the
 * input/output layer changes, from a blocking read(0,...) text
 * prompt to gui_recv_event()-delivered key_events (only ever sent
 * while this window holds focus) drawn into a libgui.so surface.
 *
 * Restyled (ToaruOS-style GUI rewrite) to match ToaruOS's own
 * glogin-provider.c: a centered rounded translucent "glass" box over a
 * blurred crop of the desktop wallpaper, antialiased DejaVu Sans/Sans-
 * Bold text (gui/libgui/ttf.c), a cyan-blue focus ring on the active
 * field, and a masked password field. The compositor has no real
 * per-pixel alpha compositing (see gui/compositor.c's own comment on
 * why the desktop background is compositor-drawn, not a client
 * surface) - the "glass" effect is faked entirely within this client's
 * own opaque committed surface: it independently loads and blurs the
 * same wallpaper asset, crops exactly the region behind its own
 * (self-chosen, centered) screen position, and blends its translucent
 * box on top of that local copy before ever committing.
 *
 * On successful auth: setgid()+setuid() to the authenticated account
 * (group before user - dropping uid away from root also drops the
 * permission to change gid, same ordering login.c's own comment
 * explains), then exec()s the desktop shell (/usr/bin/desktop,
 * gui/desktop.c) as that now-unprivileged uid, connecting to the
 * *same running* compositor as a new client - the compositor itself
 * never restarts or changes uid, matching how a real display server
 * persists across login/logout. Never returns except by _exit() on an
 * exec failure, same convention as login.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"
#include "ttf.h"

#define WIN_W 360
#define WIN_H 220
#define FIELD_MAX 31

#define BOX_MARGIN 10
#define BOX_RADIUS 14
#define FIELD_H    30
#define FIELD_RADIUS 8

#define COLOR_GLASS_TINT   0x101418
#define GLASS_ALPHA        150
#define COLOR_TITLE        0xFFFFFF
#define COLOR_LABEL        0xA8B0B8
#define COLOR_FIELD_BG     0xF4F4F4
#define COLOR_FIELD_TEXT   0x202020
#define COLOR_FIELD_UNFOCUS 0x9EA9B1
#define COLOR_FIELD_FOCUS   0x08C1EC
#define COLOR_ERR          0xF01414
#define COLOR_FOOTER       0xE8E8E8

enum { ST_USER, ST_PASS };

struct login_ui {
	struct gui_conn c;
	struct gfx_surface wallpaper;   /* independently loaded, own format */
	struct ttf_font *sans, *sans_bold;
	int winx, winy;                 /* our own chosen screen position */
};

/* Fills the whole window with a seamless base layer: the sharp
 * wallpaper crop matching our real screen position (so the window's
 * own edges are invisible against the desktop), then a blurred crop
 * pasted under just the glass box's bounding rect, then the
 * translucent rounded tint on top. Called once up front - text/fields
 * are drawn per-frame on top of this already-composited background by
 * render(), not recomputed each time (the blur is the expensive part).
 */
static void
paint_background(struct login_ui *ui)
{
	struct gfx_surface blurbuf;

	if (ui->wallpaper.pixels) {
		gfx_blit(&ui->c.surface, 0, 0, &ui->wallpaper, ui->winx, ui->winy, WIN_W, WIN_H);

		blurbuf = ui->c.surface;
		blurbuf.pixels = malloc((unsigned long)ui->c.surface.pitch * WIN_H);
		if (blurbuf.pixels) {
			blurbuf.w = WIN_W;
			blurbuf.h = WIN_H;
			gfx_blit(&blurbuf, 0, 0, &ui->wallpaper, ui->winx, ui->winy, WIN_W, WIN_H);
			gfx_box_blur(&blurbuf, 8);
			gfx_blit(&ui->c.surface, BOX_MARGIN, BOX_MARGIN, &blurbuf,
			         BOX_MARGIN, BOX_MARGIN, WIN_W - 2 * BOX_MARGIN, WIN_H - 2 * BOX_MARGIN);
			free(blurbuf.pixels);
		}
	} else {
		gfx_fill_rect(&ui->c.surface, 0, 0, WIN_W, WIN_H, 0x181818);
	}

	gfx_fill_rounded_rect_alpha(&ui->c.surface, BOX_MARGIN, BOX_MARGIN,
	                             WIN_W - BOX_MARGIN, WIN_H - BOX_MARGIN,
	                             BOX_RADIUS, COLOR_GLASS_TINT, GLASS_ALPHA);
}

static void
draw_field(struct login_ui *ui, int y, const char *text, int focused, int masked)
{
	int x0 = BOX_MARGIN + 24, x1 = WIN_W - BOX_MARGIN - 24;
	unsigned int ring = focused ? COLOR_FIELD_FOCUS : COLOR_FIELD_UNFOCUS;
	char masked_buf[FIELD_MAX + 1];

	gfx_fill_rounded_rect(&ui->c.surface, x0 - 2, y - 2, x1 + 2, y + FIELD_H + 2, FIELD_RADIUS + 2, ring);
	gfx_fill_rounded_rect(&ui->c.surface, x0, y, x1, y + FIELD_H, FIELD_RADIUS, COLOR_FIELD_BG);

	if (masked) {
		unsigned long i, n = strlen(text);

		if (n > FIELD_MAX)
			n = FIELD_MAX;
		for (i = 0; i < n; i++)
			masked_buf[i] = '*';
		masked_buf[n] = 0;
		text = masked_buf;
	}
	ttf_draw_string(&ui->c.surface, ui->sans, x0 + 12, y + 7, text, 14, COLOR_FIELD_TEXT);
	if (focused) {
		int caret_x = x0 + 12 + ttf_string_width(ui->sans, text, 14) + 1;

		gfx_fill_rect(&ui->c.surface, caret_x, y + 6, caret_x + 2, y + FIELD_H - 6, COLOR_FIELD_TEXT);
	}
}

static void
render(struct login_ui *ui, int state, const char *user, const char *pass, const char *msg)
{
	int box_top = BOX_MARGIN;
	int title_y = box_top + 14;
	int field1_y = box_top + 58;
	int field2_y = field1_y + FIELD_H + 30;

	paint_background(ui);

	ttf_draw_string(&ui->c.surface, ui->sans_bold,
	                 WIN_W / 2 - ttf_string_width(ui->sans_bold, "poc-os", 22) / 2,
	                 title_y, "poc-os", 22, COLOR_TITLE);

	ttf_draw_string(&ui->c.surface, ui->sans, BOX_MARGIN + 24, field1_y - 18, "Username", 12, COLOR_LABEL);
	draw_field(ui, field1_y, user, state == ST_USER, 0);

	ttf_draw_string(&ui->c.surface, ui->sans, BOX_MARGIN + 24, field2_y - 18, "Password", 12, COLOR_LABEL);
	draw_field(ui, field2_y, pass, state == ST_PASS, 1);

	if (msg) {
		int w = ttf_string_width(ui->sans, msg, 13);

		ttf_draw_string(&ui->c.surface, ui->sans, WIN_W / 2 - w / 2, WIN_H - 34, msg, 13, COLOR_ERR);
	} else {
		int w = ttf_string_width(ui->sans, "poc-os", 13);

		ttf_draw_string_shadow(&ui->c.surface, ui->sans, WIN_W / 2 - w / 2, WIN_H - 32,
		                        "poc-os", 13, COLOR_FOOTER, 0x000000);
	}

	gui_commit(&ui->c);
}

int
main(void)
{
	struct login_ui ui;
	char user[FIELD_MAX + 1] = { 0 };
	char pass[FIELD_MAX + 1] = { 0 };
	int state = ST_USER;
	struct gui_event ev;
	struct passwd *pw;

	memset(&ui, 0, sizeof(ui));

	{
		int tries = 0;

		while (gui_connect(&ui.c, GUI_SOCK_PATH) < 0) {
			if (++tries > 100000) {
				printf("login_gui: gui_connect failed\n");
				return 1;
			}
		}
	}

	/* Probe placement first (auto-cascade) purely to learn the real
	 * screen size from the reply, then destroy and recreate centered -
	 * the wire protocol has no separate "query screen size" message,
	 * and a client can't compute a centered position without first
	 * knowing what it's centering within. See gui_proto.h's own
	 * comment on gui_msg_surface_created's screen_w/screen_h. */
	if (gui_create_surface(&ui.c, WIN_W, WIN_H, -1, -1, GUI_WIN_BORDERLESS, "login") < 0) {
		printf("login_gui: gui_create_surface (probe) failed\n");
		return 1;
	}
	ui.winx = (int)(ui.c.screen_w - WIN_W) / 2;
	ui.winy = (int)(ui.c.screen_h - WIN_H) / 2;
	gui_destroy(&ui.c);
	if (gui_connect(&ui.c, GUI_SOCK_PATH) < 0 ||
	    gui_create_surface(&ui.c, WIN_W, WIN_H, ui.winx, ui.winy, GUI_WIN_BORDERLESS, "login") < 0) {
		printf("login_gui: gui_create_surface (final) failed\n");
		return 1;
	}

	gfx_load_raw(&ui.wallpaper, "/usr/share/wallpaper.raw", &ui.c.surface);
	ui.sans = ttf_load("/usr/share/fonts/dejavu/DejaVuSans.ttf");
	ui.sans_bold = ttf_load("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf");
	if (!ui.sans || !ui.sans_bold) {
		printf("login_gui: font load failed\n");
		return 1;
	}

	render(&ui, state, user, pass, 0);

	for (;;) {
		int ulen, plen;

		if (gui_recv_event(&ui.c, &ev) < 0) {
			printf("login_gui: connection closed\n");
			return 1;
		}
		if (ev.type != GUI_EVENT_KEY)
			continue;

		ulen = (int)strlen(user);
		plen = (int)strlen(pass);

		if (ev.key.ch == 0x7f || ev.key.ch == 0x08) {
			if (state == ST_USER && ulen > 0)
				user[ulen - 1] = 0;
			else if (state == ST_PASS && plen > 0)
				pass[plen - 1] = 0;
			render(&ui, state, user, pass, 0);
			continue;
		}

		if (ev.key.ch == '\t' && state == ST_USER) {
			state = ST_PASS;
			render(&ui, state, user, pass, 0);
			continue;
		}

		if (ev.key.ch == '\r' || ev.key.ch == '\n') {
			if (state == ST_USER) {
				if (ulen > 0)
					state = ST_PASS;
				render(&ui, state, user, pass, 0);
				continue;
			}

			/* state == ST_PASS: attempt authentication. */
			pw = getpwnam(user);
			if (pw == NULL || strcmp(pw->pw_passwd, pass) != 0) {
				user[0] = 0;
				pass[0] = 0;
				state = ST_USER;
				render(&ui, state, user, pass, "Incorrect username or password.");
				continue;
			}

			/* Group before user: dropping uid away from root
			 * also drops the permission to change gid (see
			 * kernel/sysproc.c's sys_setuid() comment - the
			 * same POSIX ordering login.c's own text-console
			 * version already follows). */
			if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
				render(&ui, state, user, pass, "Identity switch failed.");
				continue;
			}
			chdir(pw->pw_dir);

			{
				char *argv[] = { "desktop", 0 };
				char *envp[] = { 0 };

				gui_destroy(&ui.c);
				execve("/usr/bin/desktop", argv, envp);
				printf("login_gui: exec desktop failed\n");
				_exit(1);
			}
		}

		if (ev.key.ch >= 0x20 && ev.key.ch < 0x7f) {
			if (state == ST_USER && ulen < FIELD_MAX) {
				user[ulen] = (char)ev.key.ch;
				user[ulen + 1] = 0;
			} else if (state == ST_PASS && plen < FIELD_MAX) {
				pass[plen] = (char)ev.key.ch;
				pass[plen + 1] = 0;
			}
			render(&ui, state, user, pass, 0);
		}
	}
}
