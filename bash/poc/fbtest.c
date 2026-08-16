/* fbtest: a throwaway diagnostic for the new VBE linear-framebuffer
 * driver (boot/boot2_bios.asm's setup_vbe, kernel/vbe.c, kernel/
 * sysproc.c's sys_mmap()/sys_ioctl() FRAMEBUFFER paths) - not a real
 * feature, just proof the whole chain (boot-time mode set -> kernel
 * validation -> device node -> ioctl geometry query -> mmap of the
 * real physical VRAM) actually works end to end: draws horizontal
 * color bars spanning the full width, unambiguous in a screendump
 * (unlike a single solid fill, which could just as easily be leftover
 * VRAM content from before this program ran). Same dynamic
 * Scrt1.o+libc.so PIE build as rawtest.c/curses_test.c before it - see
 * this file's own Makefile rule.
 *
 * The framebuffer device node ("framebuffer", root-level - no /dev
 * directory convention exists in this codebase, "console" lives
 * directly at /console too) is created lazily on first open() failure,
 * the same self-mknod dance bash/poc/dinit.c already does for
 * "console".
 *
 * Usage: run from bash after logging in (needs a VBE-capable boot -
 * see kernel/vbe.c's own boot-log line for whether one was found).
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

extern long syscall(long, ...);

#define FRAMEBUFFER_MAJOR 2
#define FBIOGET_VSCREENINFO 1

struct fb_info {
	unsigned int xres;
	unsigned int yres;
	unsigned int pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

static unsigned int
pack(struct fb_info *fi, unsigned int r, unsigned int g, unsigned int b)
{
	return (r << fi->red_field_pos) | (g << fi->green_field_pos) |
	       (b << fi->blue_field_pos);
}

int
main(void)
{
	int fd;
	struct fb_info fi;
	unsigned char *fb;
	unsigned int colors[5];
	unsigned int band, x, y;

	fd = open("framebuffer", O_RDWR);
	if (fd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fd = open("framebuffer", O_RDWR);
	}
	if (fd < 0) {
		printf("fbtest: open failed\n");
		return 1;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("fbtest: no usable framebuffer (VBE mode not set at boot?)\n");
		return 1;
	}
	printf("fbtest: %ux%u %ubpp, pitch %u\n", fi.xres, fi.yres, fi.bpp, fi.pitch);

	fb = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		printf("fbtest: mmap failed\n");
		return 1;
	}

	colors[0] = pack(&fi, 255, 0, 0);     /* red */
	colors[1] = pack(&fi, 0, 255, 0);     /* green */
	colors[2] = pack(&fi, 0, 0, 255);     /* blue */
	colors[3] = pack(&fi, 255, 255, 255); /* white */
	colors[4] = pack(&fi, 0, 0, 0);       /* black */

	for (y = 0; y < fi.yres; y++) {
		band = (y * 5) / fi.yres;
		unsigned char *row = fb + (unsigned long)y * fi.pitch;
		for (x = 0; x < fi.xres; x++)
			*(unsigned int *)(row + x * 4) = colors[band];
	}

	printf("fbtest: drew color bars\n");
	return 0;
}
