/* mousetest: throwaway diagnostic for the PS/2 mouse driver
 * (kernel/mouse.c - GUI roadmap phase 3). Opens (self-mknod's if
 * needed) "mouse" the same way fbtest.c already does for
 * "framebuffer", then blocks reading packets and prints each one's
 * decoded dx/dy/buttons - verified by injecting real PS/2 events via
 * QEMU's monitor mouse_move/mouse_button HMP commands while this is
 * running (see the plan doc's verification section). Same dynamic
 * Scrt1.o+libc.so PIE build as fbtest.c before it.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

extern long syscall(long, ...);

#define MOUSE_MAJOR 3

struct mousepkt {
	unsigned char buttons;
	signed char dx, dy;
};

int
main(void)
{
	int fd, i;
	struct mousepkt pkt;

	fd = open("mouse", O_RDONLY);
	if (fd < 0) {
		syscall(SYS_mknod, "mouse", MOUSE_MAJOR, 0);
		fd = open("mouse", O_RDONLY);
	}
	if (fd < 0) {
		printf("mousetest: open failed\n");
		return 1;
	}

	printf("mousetest: waiting for packets...\n");
	for (i = 0; i < 5; i++) {
		if (read(fd, &pkt, sizeof(pkt)) != (int)sizeof(pkt)) {
			printf("mousetest: read failed\n");
			return 1;
		}
		printf("mousetest: pkt %d: buttons=0x%x dx=%d dy=%d\n",
		       i, pkt.buttons, pkt.dx, pkt.dy);
	}
	printf("mousetest: done\n");
	return 0;
}
