/* login_gui: GUI roadmap phase 9 - a graphical login screen, launched
 * as the compositor's first client (gui/compositor.c starts as root
 * at boot, before any user session exists). Reuses the *existing*
 * authentication logic from bash/poc/login.c (getpwnam() against the
 * real /etc/passwd, phase 1's permcheck infra) verbatim - only the
 * input/output layer changes, from a blocking read(0,...) text
 * prompt to gui_recv_event()-delivered key_events (only ever sent
 * while this window holds focus) drawn into a libgui.so surface.
 *
 * On successful auth: setgid()+setuid() to the authenticated account
 * (group before user - dropping uid away from root also drops the
 * permission to change gid, same ordering login.c's own comment
 * explains), then exec()s GUI roadmap phase 10's real terminal
 * (/usr/bin/terminal, gui/terminal.c) as that now-unprivileged uid,
 * connecting to the *same running* compositor as a new client - the
 * compositor itself never restarts or changes uid, matching how a
 * real display server persists across login/logout. Never returns
 * except by _exit() on an exec failure, same convention as login.c.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"

#define WIN_W 320
#define WIN_H 140
#define FIELD_MAX 31

#define COLOR_BG     0x202830
#define COLOR_LABEL  0xA0A8B0
#define COLOR_FIELD  0xFFFFFF
#define COLOR_ERR    0xFF6060
#define COLOR_TITLE  0xFFFFFF

enum { ST_USER, ST_PASS };

static void
render(struct gui_conn *c, int state, const char *user, const char *pass_mask, const char *msg)
{
	gfx_fill_rect(&c->surface, 0, 0, WIN_W, WIN_H, COLOR_BG);
	gfx_draw_string(&c->surface, 12, 12, "poc-os login", COLOR_TITLE);

	gfx_draw_string(&c->surface, 12, 40, "user:", COLOR_LABEL);
	gfx_draw_string(&c->surface, 80, 40, user, COLOR_FIELD);
	if (state == ST_USER)
		gfx_draw_string(&c->surface, 80 + 8 * (int)strlen(user), 40, "_", COLOR_FIELD);

	gfx_draw_string(&c->surface, 12, 60, "pass:", COLOR_LABEL);
	gfx_draw_string(&c->surface, 80, 60, pass_mask, COLOR_FIELD);
	if (state == ST_PASS)
		gfx_draw_string(&c->surface, 80 + 8 * (int)strlen(pass_mask), 60, "_", COLOR_FIELD);

	if (msg)
		gfx_draw_string(&c->surface, 12, 90, msg, COLOR_ERR);

	gui_commit(c);
}

int
main(void)
{
	struct gui_conn c;
	char user[FIELD_MAX + 1] = { 0 };
	char pass[FIELD_MAX + 1] = { 0 };
	char mask[FIELD_MAX + 1] = { 0 };
	int state = ST_USER;
	struct gui_event ev;
	struct passwd *pw;

	{
		int tries = 0;

		while (gui_connect(&c, GUI_SOCK_PATH) < 0) {
			if (++tries > 100000) {
				printf("login_gui: gui_connect failed\n");
				return 1;
			}
		}
	}
	if (gui_create_surface(&c, WIN_W, WIN_H, "login") < 0) {
		printf("login_gui: gui_create_surface failed\n");
		return 1;
	}

	render(&c, state, user, mask, 0);

	for (;;) {
		int ulen, plen;

		if (gui_recv_event(&c, &ev) < 0) {
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
			else if (state == ST_PASS && plen > 0) {
				pass[plen - 1] = 0;
				mask[plen - 1] = 0;
			}
			render(&c, state, user, mask, 0);
			continue;
		}

		if (ev.key.ch == '\r' || ev.key.ch == '\n') {
			if (state == ST_USER) {
				if (ulen > 0)
					state = ST_PASS;
				render(&c, state, user, mask, 0);
				continue;
			}

			/* state == ST_PASS: attempt authentication. */
			pw = getpwnam(user);
			if (pw == NULL || strcmp(pw->pw_passwd, pass) != 0) {
				user[0] = 0;
				pass[0] = 0;
				mask[0] = 0;
				state = ST_USER;
				render(&c, state, user, mask, "login incorrect");
				continue;
			}

			/* Group before user: dropping uid away from root
			 * also drops the permission to change gid (see
			 * kernel/sysproc.c's sys_setuid() comment - the
			 * same POSIX ordering login.c's own text-console
			 * version already follows). */
			if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
				render(&c, state, user, mask, "identity switch failed");
				continue;
			}
			chdir(pw->pw_dir);

			{
				char *argv[] = { "terminal", 0 };
				char *envp[] = { 0 };

				gui_destroy(&c);
				execve("/usr/bin/terminal", argv, envp);
				printf("login_gui: exec terminal failed\n");
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
				mask[plen] = '*';
				mask[plen + 1] = 0;
			}
			render(&c, state, user, mask, 0);
		}
	}
}
