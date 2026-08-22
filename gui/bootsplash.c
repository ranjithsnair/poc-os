/* bootsplash: draws a "poc-os" splash screen directly to the real
 * framebuffer and exits immediately - no compositor, no event loop,
 * no socket. bash/poc/dinit.c runs this (and waits for it to exit)
 * *before* forking gui/compositor.c, so this is the first thing a
 * user ever sees on the real screen, and it stays on screen untouched
 * (compositor.c no longer paints anything at its own startup - see
 * its own comment) until gui/login_gui.c's first gui_commit() replaces
 * it with the login box.
 *
 * Deliberately draws everything procedurally (solid fill + the same
 * built-in bitmap font gui/compositor.c already uses for titlebar
 * text, scaled up locally below) instead of loading a raw image asset
 * the way gui/compositor.c's wallpaper or gui/desktop.c's icon do:
 * this used to gfx_load_raw() a 1024x768 (~2.3MB) asset, and reading
 * that through this filesystem's readi() - one BSIZE=512-byte block
 * at a time, kernel/fs.c's own loop, no bulk-read path - took long
 * enough under this kernel's software-emulated IDE path to be the
 * actual "why is there a delay before the boot screen even shows up"
 * complaint; a plain solid fill plus a few thousand fill_rect() calls
 * for scaled glyphs costs no disk I/O at all. Still visually distinct
 * from the desktop wallpaper photo (gui/assets/images/wallpaper.raw),
 * just as a vector logo rather than another raster image.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "gfx.h"
#include "font8x16.h"

extern long syscall(long, ...);

#define FRAMEBUFFER_MAJOR 2
#define FBIOGET_VSCREENINFO 1

#define COLOR_BG     0x0B0E12 /* dark navy - distinct from the desktop wallpaper photo */
#define COLOR_TEXT   0xE2E2E2 /* same ACTIVE_COLOR gui/compositor.c's titlebar text uses */
#define COLOR_ACCENT 0x5DA3EC /* same accent blue gui/desktop.c's top bar gradient uses */

#define TEXT_SCALE 4 /* each 8x16 glyph becomes (8*SCALE)x(16*SCALE) px */

struct fb_info {
	unsigned int xres, yres, pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

// Manual pixel-doubled glyph blit - gui/libgui/gfx.h's gfx_draw_string()
// only ever draws at the font's native 8x16 size (used as-is for
// titlebar/UI text elsewhere), so a bigger "logo" size needs its own
// small scaling loop here rather than a shared library change just
// for this one caller.
static void
draw_char_scaled(struct gfx_surface *s, int x, int y, char ch, unsigned int color, int scale)
{
	const unsigned char *rows;
	unsigned int px;
	int row, col, sx, sy;

	if (ch < 32 || ch > 126)
		return;
	rows = font8x16[(unsigned char)ch - 32];
	px = gfx_pack(s, color);

	for (row = 0; row < FONT_CELL_H; row++) {
		unsigned char bits = rows[row];

		for (col = 0; col < FONT_CELL_W; col++) {
			if (!(bits & (0x80 >> col)))
				continue;
			for (sy = 0; sy < scale; sy++) {
				int py = y + row * scale + sy;

				if (py < 0 || py >= (int)s->h)
					continue;
				for (sx = 0; sx < scale; sx++) {
					int pxx = x + col * scale + sx;

					if (pxx < 0 || pxx >= (int)s->w)
						continue;
					*(unsigned int *)(s->pixels + (unsigned long)py * s->pitch + (unsigned long)pxx * 4) = px;
				}
			}
		}
	}
}

static void
draw_string_scaled(struct gfx_surface *s, int x, int y, const char *str, unsigned int color, int scale)
{
	int cx = x;

	for (; *str; str++) {
		draw_char_scaled(s, cx, y, *str, color, scale);
		cx += FONT_CELL_W * scale;
	}
}

int
main(void)
{
	int fbfd;
	struct fb_info fi;
	struct gfx_surface fbsurf;
	const char *title = "poc-os";
	int title_w, title_x, title_y, bar_y;

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

	gfx_fill_rect(&fbsurf, 0, 0, (int)fbsurf.w, (int)fbsurf.h, COLOR_BG);

	title_w = (int)(FONT_CELL_W * TEXT_SCALE) * (int)strlen(title);
	title_x = ((int)fbsurf.w - title_w) / 2;
	title_y = ((int)fbsurf.h - FONT_CELL_H * TEXT_SCALE) / 2 - 20;
	draw_string_scaled(&fbsurf, title_x, title_y, title, COLOR_TEXT, TEXT_SCALE);

	bar_y = title_y + FONT_CELL_H * TEXT_SCALE + 24;
	gfx_fill_rect(&fbsurf, title_x, bar_y, title_x + title_w, bar_y + 4, COLOR_ACCENT);

	return 0;
}
