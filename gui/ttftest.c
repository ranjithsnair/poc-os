// Temporary standalone test: draws directly to the real framebuffer
// (bypassing the compositor entirely, same open/mknod-fallback/ioctl/
// mmap sequence compositor.c uses) to verify gui/libgui/ttf.c's
// TrueType pipeline actually produces correct antialiased glyphs under
// the real kernel/FPU/filesystem, before it's wired into the real
// login/desktop/terminal screens. Not installed by the Makefile /
// meant to be deleted once verified - see the GUI roadmap ToaruOS-
// style rewrite plan.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "gfx.h"
#include "ttf.h"

#define FRAMEBUFFER_MAJOR 2
#define FBIOGET_VSCREENINFO 1

struct fb_info {
	unsigned int xres, yres, pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

extern long syscall(long, ...);

int
main(void)
{
	int fbfd;
	struct fb_info fi;
	struct gfx_surface fb;
	struct ttf_font *sans, *sansbold, *mono;

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("ttftest: no usable framebuffer\n");
		return 1;
	}
	fb.pixels = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb.pixels == MAP_FAILED) {
		printf("ttftest: mmap failed\n");
		return 1;
	}
	fb.w = fi.xres;
	fb.h = fi.yres;
	fb.pitch = fi.pitch;
	fb.red_mask_size = fi.red_mask_size;
	fb.red_field_pos = fi.red_field_pos;
	fb.green_mask_size = fi.green_mask_size;
	fb.green_field_pos = fi.green_field_pos;
	fb.blue_mask_size = fi.blue_mask_size;
	fb.blue_field_pos = fi.blue_field_pos;

	gfx_fill_rect(&fb, 0, 0, (int)fb.w, (int)fb.h, 0x202830);

	sans = ttf_load("usr/share/fonts/dejavu/DejaVuSans.ttf");
	sansbold = ttf_load("usr/share/fonts/dejavu/DejaVuSans-Bold.ttf");
	mono = ttf_load("usr/share/fonts/dejavu/DejaVuSansMono.ttf");
	printf("ttftest: sans=%p sansbold=%p mono=%p\n", (void*)sans, (void*)sansbold, (void*)mono);

	if (sans)
		ttf_draw_string(&fb, sans, 40, 40, "The quick brown fox jumps 0123456789", 24, 0xFFFFFF);
	if (sansbold)
		ttf_draw_string(&fb, sansbold, 40, 90, "Bold: poc-os login", 20, 0xA0C8FF);
	if (mono)
		ttf_draw_string(&fb, mono, 40, 140, "mono: user@poc-os:~$ ls -la", 18, 0x30D030);
	if (sans)
		ttf_draw_string_shadow(&fb, sans, 40, 190, "Shadowed text over dark bg", 22, 0xFFFFFF, 0x000000);

	gfx_fill_rounded_rect(&fb, 40, 240, 340, 320, 12, 0x2A3540);
	if (sans)
		ttf_draw_string(&fb, sans, 60, 260, "Rounded box test", 18, 0xFFFFFF);

	gfx_fill_rect_gradient(&fb, 40, 340, 340, 380, 0x5DA3EC, 0x3889DC);

	printf("ttftest: done, sleeping\n");
	sleep(30);
	return 0;
}
