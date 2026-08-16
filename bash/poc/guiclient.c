/* guiclient: throwaway diagnostic for GUI roadmap phase 8 - an
 * ordinary libgui.so client (unlike bash/poc/libguitest.c's Phase 7
 * test, this one connects to the *real* compositor, gui/compositor.c,
 * not a stub) used to prove multi-window compositing, z-order,
 * focus-raise, and drag-to-move all work against real synthetic
 * mouse/keyboard input. Draws a solid-color rect + its own title as a
 * label, then loops printing every input event it receives (only
 * delivered - for key events - while it holds focus) so a captured
 * serial log confirms focus routing is correct, not just that
 * something rendered.
 *
 * Usage: guiclient <w> <h> <0xRRGGBB color> <title>
 */
#include <stdio.h>
#include <stdlib.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"

int
main(int argc, char **argv)
{
	struct gui_conn c;
	int w, h;
	unsigned int color;
	const char *title;
	struct gui_event ev;

	if (argc != 5) {
		printf("guiclient: usage: guiclient <w> <h> <0xRRGGBB> <title>\n");
		return 1;
	}
	w = atoi(argv[1]);
	h = atoi(argv[2]);
	color = (unsigned int)strtoul(argv[3], 0, 16);
	title = argv[4];

	{
		int tries = 0;

		while (gui_connect(&c, GUI_SOCK_PATH) < 0) {
			if (++tries > 2000000) {
				printf("guiclient: gui_connect failed\n");
				return 1;
			}
			/* Compositor may not have bound its listening socket
			 * yet if we were fork()'d at roughly the same time -
			 * short busy-spin retry rather than depending on a
			 * real sleep()/usleep() syscall this codebase hasn't
			 * proven yet. */
		}
	}
	if (gui_create_surface(&c, w, h, title) < 0) {
		printf("guiclient: gui_create_surface failed\n");
		return 1;
	}

	gfx_fill_rect(&c.surface, 0, 0, w, h, color);
	gfx_draw_string(&c.surface, 8, 8, title, 0xFFFFFF);

	if (gui_commit(&c) < 0) {
		printf("guiclient: gui_commit failed\n");
		return 1;
	}
	printf("guiclient[%s]: mapped and committed\n", title);

	for (;;) {
		if (gui_recv_event(&c, &ev) < 0) {
			printf("guiclient[%s]: connection closed\n", title);
			break;
		}
		switch (ev.type) {
		case GUI_EVENT_FOCUS:
			printf("guiclient[%s]: focus=%d\n", title, ev.focus.focused);
			break;
		case GUI_EVENT_KEY:
			printf("guiclient[%s]: key=0x%02x\n", title, ev.key.ch);
			break;
		case GUI_EVENT_POINTER:
			/* Frequent - not printed to keep the serial log readable. */
			break;
		}
	}
	return 0;
}
