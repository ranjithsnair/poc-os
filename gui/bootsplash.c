/* bootsplash: draws a static "poc-os" splash image directly to the
 * real framebuffer and exits immediately - no compositor, no event
 * loop, no socket. bash/poc/dinit.c runs this (and waits for it to
 * exit) *before* forking gui/compositor.c, so this is the first thing
 * a user ever sees on the real screen, and it stays on screen
 * untouched (compositor.c no longer paints anything at its own
 * startup - see its own comment) until gui/login_gui.c's first
 * gui_commit() replaces it with the login box.
 *
 * Same open/mknod-fallback/ioctl/mmap framebuffer-acquisition sequence
 * as gui/compositor.c's own main() (and fbtest.c/guitest.c before
 * it) - deliberately not shared code (this program owns the fb for
 * only the instant it takes to draw one frame, then exits; sharing a
 * "framebuffer" helper across the compositor and this one-shot program
 * isn't worth a new library for two callers, matching how struct
 * fb_info itself is duplicated rather than shared - see compositor.c's
 * own comment on that).
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "gfx.h"

extern long syscall(long, ...);

#define FRAMEBUFFER_MAJOR 2
#define FBIOGET_VSCREENINFO 1

#define SPLASH_PATH "/usr/share/bootsplash.raw"
#define COLOR_BG 0x0B0E12 /* matches the splash image's own background,
                            * used only if the asset is missing or its
                            * resolution doesn't match the real mode */

struct fb_info {
	unsigned int xres, yres, pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

int
main(void)
{
	int fbfd;
	struct fb_info fi;
	struct gfx_surface fbsurf, splash;

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0)
		return 1;

	fbsurf.pixels = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbsurf.pixels == MAP_FAILED)
		return 1;
	fbsurf.w = fi.xres;
	fbsurf.h = fi.yres;
	fbsurf.pitch = fi.pitch;
	fbsurf.red_mask_size = fi.red_mask_size;
	fbsurf.red_field_pos = fi.red_field_pos;
	fbsurf.green_mask_size = fi.green_mask_size;
	fbsurf.green_field_pos = fi.green_field_pos;
	fbsurf.blue_mask_size = fi.blue_mask_size;
	fbsurf.blue_field_pos = fi.blue_field_pos;

	gfx_load_raw(&splash, SPLASH_PATH, &fbsurf);
	if (splash.pixels && splash.w == fbsurf.w && splash.h == fbsurf.h)
		gfx_blit(&fbsurf, 0, 0, &splash, 0, 0, (int)fbsurf.w, (int)fbsurf.h);
	else
		gfx_fill_rect(&fbsurf, 0, 0, (int)fbsurf.w, (int)fbsurf.h, COLOR_BG);

	return 0;
}
