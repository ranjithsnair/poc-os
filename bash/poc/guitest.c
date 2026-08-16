/* guitest: throwaway diagnostic for a real, interactive GUI element
 * (GUI roadmap phase 5) - one clickable button, hit-tested against
 * real PS/2 mouse input (kernel/mouse.c, phase 3) and redrawn onto
 * the real VBE framebuffer (kernel/vbe.c, phase 2) on every event.
 * Deliberately standalone (owns the framebuffer/mouse directly, not
 * split across the Wayland client/compositor relationship from phases
 * 3-4) and text-free (no bitmap font yet - state changes are shown
 * through color instead) - both named scope cuts for this phase, not
 * permanent limits. See the plan doc for the full rationale:
 * /Users/ranjith/.claude/plans/structured-stargazing-pixel.md
 *
 * cy tracking negates the packet's dy (cy -= pkt.dy, not +=): real
 * PS/2 packets report positive Y as "up", confirmed empirically during
 * phase 3's mousetest.c verification (QEMU's own "mouse_move dx dy"
 * HMP command uses the opposite, screen-down-positive convention) -
 * screen rows increase downward, so "up" has to decrease cy.
 *
 * Same dynamic Scrt1.o+libc.so PIE build as fbtest.c/mousetest.c
 * before it - see this file's own Makefile rule.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

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

#define BTN_X0 400
#define BTN_Y0 300
#define BTN_X1 600
#define BTN_Y1 380

#define CURSOR_HALF 5
#define MAX_PACKETS 500
#define TARGET_CLICKS 3

static unsigned int
pack(struct fb_info *fi, unsigned int color)
{
	unsigned int r = (color >> 16) & 0xff;
	unsigned int g = (color >> 8) & 0xff;
	unsigned int b = color & 0xff;

	return (r << fi->red_field_pos) | (g << fi->green_field_pos) | (b << fi->blue_field_pos);
}

static void
fill_rect(unsigned char *fb, struct fb_info *fi, int x0, int y0, int x1, int y1, unsigned int px)
{
	int x, y;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)fi->xres) x1 = fi->xres;
	if (y1 > (int)fi->yres) y1 = fi->yres;

	for (y = y0; y < y1; y++) {
		unsigned char *row = fb + (unsigned long)y * fi->pitch;

		for (x = x0; x < x1; x++)
			*(unsigned int *)(row + x * 4) = px;
	}
}

static void
redraw(unsigned char *fb, struct fb_info *fi, unsigned int button_color, int cx, int cy)
{
	fill_rect(fb, fi, 0, 0, fi->xres, fi->yres, pack(fi, 0x202020));
	fill_rect(fb, fi, BTN_X0, BTN_Y0, BTN_X1, BTN_Y1, pack(fi, button_color));
	fill_rect(fb, fi, cx - CURSOR_HALF, cy - CURSOR_HALF, cx + CURSOR_HALF, cy + CURSOR_HALF,
	          pack(fi, 0xFFFFFF));
}

int
main(void)
{
	int mfd, fbfd, i;
	struct fb_info fi;
	unsigned char *fb;
	struct mousepkt pkt;
	int cx, cy, prev_pressed;
	int clicks;
	unsigned int colors[4] = { 0x808080, 0x0000FF, 0x00FF00, 0xFF0000 };
	const char *names[4] = { "gray", "blue", "green", "red" };

	mfd = open("mouse", O_RDONLY);
	if (mfd < 0) {
		syscall(SYS_mknod, "mouse", MOUSE_MAJOR, 0);
		mfd = open("mouse", O_RDONLY);
	}
	if (mfd < 0) {
		printf("guitest: mouse open failed\n");
		return 1;
	}

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("guitest: no usable framebuffer\n");
		return 1;
	}
	fb = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == MAP_FAILED) {
		printf("guitest: framebuffer mmap failed\n");
		return 1;
	}

	cx = (int)fi.xres / 2;
	cy = (int)fi.yres / 2;
	prev_pressed = 0;
	clicks = 0;
	redraw(fb, &fi, colors[0], cx, cy);
	printf("guitest: waiting for clicks on button (%d,%d)-(%d,%d)\n",
	       BTN_X0, BTN_Y0, BTN_X1, BTN_Y1);

	for (i = 0; i < MAX_PACKETS && clicks < TARGET_CLICKS; i++) {
		int pressed_now;

		if (read(mfd, &pkt, sizeof(pkt)) != (int)sizeof(pkt)) {
			printf("guitest: mouse read failed\n");
			return 1;
		}

		cx += pkt.dx;
		cy -= pkt.dy;
		if (cx < 0) cx = 0;
		if (cx >= (int)fi.xres) cx = fi.xres - 1;
		if (cy < 0) cy = 0;
		if (cy >= (int)fi.yres) cy = fi.yres - 1;

		pressed_now = pkt.buttons & 0x1;
		if (pressed_now && !prev_pressed &&
		    cx >= BTN_X0 && cx < BTN_X1 && cy >= BTN_Y0 && cy < BTN_Y1) {
			clicks++;
			printf("guitest: click %d -> %s\n", clicks, names[clicks]);
		}
		prev_pressed = pressed_now;

		redraw(fb, &fi, colors[clicks], cx, cy);
	}

	if (clicks < TARGET_CLICKS) {
		printf("guitest: FAIL - only %d/%d clicks after %d packets\n",
		       clicks, TARGET_CLICKS, i);
		return 1;
	}
	printf("guitest: PASS - button reached final color %s\n", names[clicks]);
	return 0;
}
