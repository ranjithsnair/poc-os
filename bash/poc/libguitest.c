/* libguitest: throwaway diagnostic for GUI roadmap phase 7 - proves
 * libgui.so's client half (gui/libgui/client.c: gui_connect/
 * gui_create_surface/gui_commit) and the bespoke wire protocol
 * (gui/libgui/gui_proto.h) work end-to-end against a *minimal stub*
 * compositor (not the real one - that's Phase 8), and that the
 * generated bitmap font (gui/font8x16.h) actually renders for the
 * first time in this codebase.
 *
 * One binary, fork()'d in two: the parent plays compositor-stub
 * (opens the real framebuffer, accepts one connection, answers
 * CREATE_SURFACE with a real shm-backed surface matching the real
 * framebuffer's own pixel format, and on COMMIT blits the client's
 * drawing into the real framebuffer at a fixed offset); the child is
 * an ordinary libgui.so client (gui_connect + gui_create_surface +
 * gfx_fill_rect + gfx_draw_string + gui_commit). Same fork()-based
 * single-binary shape as bash/poc/ipctest.c's socket+shm+epoll test,
 * for the same reason: no dependency on bash's own (NOJOBS) background
 * job support.
 *
 * **Real kernel bug found and fixed during this phase**: this program
 * was the first in the whole codebase to both mmap() the framebuffer
 * *and* fork() (to spawn the client below) - kernel/vm.c's copyuvm()
 * unconditionally tried to memmove() a *copy* of every mapped page's
 * content for the child, including the framebuffer's real VRAM pages,
 * via P2V(pa) - which silently wraps around 64-bit arithmetic for a
 * physical address this high (KERNBASE + 0xfd000000 overflows to
 * 0x7d000000), producing a bogus source pointer and panicking the
 * kernel. Root-caused with a real GDB backtrace (x86_64-elf-gdb,
 * installed via Homebrew, attached to `make qemu-gdb`'s stub) after
 * printf-bisection alone couldn't pin it down. Fixed in copyuvm():
 * device memory (pa >= PHYSTOP, the same test deallocuvm() already
 * uses) is now shared with the child by mapping the same physical
 * page again, not copied - matching real mmap()'d-device-survives-
 * fork() semantics. See kernel/vm.c's own comment for the full
 * explanation.
 *
 * The blitted rectangle+text stays in real VRAM after this process
 * exits - a screendump taken any time afterward (before something
 * else draws over it) is the actual verification, same as every
 * framebuffer-touching phase before this one.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"
#include "wire.h"

extern long syscall(long, ...);

#define FRAMEBUFFER_MAJOR 2
#define FBIOGET_VSCREENINFO 1

struct fb_info {
	unsigned int xres, yres, pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

#define SURF_W 220
#define SURF_H 100
#define BLIT_X 100
#define BLIT_Y 100

static int
run_client(void)
{
	struct gui_conn c;

	if (gui_connect(&c, GUI_SOCK_PATH) < 0) {
		printf("libguitest: FAIL - client gui_connect failed\n");
		_exit(1);
	}
	if (gui_create_surface(&c, SURF_W, SURF_H, "libguitest") < 0) {
		printf("libguitest: FAIL - client gui_create_surface failed\n");
		_exit(1);
	}

	gfx_fill_rect(&c.surface, 0, 0, SURF_W, SURF_H, 0x0000A0);
	gfx_draw_string(&c.surface, 10, 10, "phase7-ok", 0xFFFFFF);

	if (gui_commit(&c) < 0) {
		printf("libguitest: FAIL - client gui_commit failed\n");
		_exit(1);
	}
	_exit(0);
}

int
main(void)
{
	int fbfd, lfd, sfd, shmfd;
	struct fb_info fi;
	unsigned char *fb;
	struct gfx_surface fbsurf, clientsurf;
	struct sockaddr_un addr;
	union gui_msg msg;
	pid_t pid;
	int status;

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("libguitest: FAIL - no usable framebuffer\n");
		return 1;
	}
	fb = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == MAP_FAILED) {
		printf("libguitest: FAIL - framebuffer mmap failed\n");
		return 1;
	}
	fbsurf.pixels = fb;
	fbsurf.w = fi.xres;
	fbsurf.h = fi.yres;
	fbsurf.pitch = fi.pitch;
	fbsurf.red_mask_size = fi.red_mask_size;
	fbsurf.red_field_pos = fi.red_field_pos;
	fbsurf.green_mask_size = fi.green_mask_size;
	fbsurf.green_field_pos = fi.green_field_pos;
	fbsurf.blue_mask_size = fi.blue_mask_size;
	fbsurf.blue_field_pos = fi.blue_field_pos;

	gfx_fill_rect(&fbsurf, 0, 0, (int)fi.xres, (int)fi.yres, 0x202020);

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("libguitest: FAIL - socket failed\n");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, GUI_SOCK_PATH);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 1) < 0) {
		printf("libguitest: FAIL - bind/listen failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("libguitest: FAIL - fork failed\n");
		return 1;
	}
	if (pid == 0)
		run_client();

	sfd = accept(lfd, 0, 0);
	if (sfd < 0) {
		printf("libguitest: FAIL - accept failed\n");
		return 1;
	}

	if (wire_recv(sfd, &msg, sizeof(msg), 0) <= 0 || msg.type != GUI_MSG_CREATE_SURFACE) {
		printf("libguitest: FAIL - CREATE_SURFACE not received\n");
		return 1;
	}

	shmfd = syscall(SYS_shm_create, (unsigned int)(msg.create_surface.w * 4 * msg.create_surface.h));
	if (shmfd < 0) {
		printf("libguitest: FAIL - shm_create failed\n");
		return 1;
	}

	{
		union gui_msg reply;

		memset(&reply, 0, sizeof(reply));
		reply.surface_created.type = GUI_MSG_SURFACE_CREATED;
		reply.surface_created.surface_id = 1;
		reply.surface_created.w = (unsigned int)msg.create_surface.w;
		reply.surface_created.h = (unsigned int)msg.create_surface.h;
		reply.surface_created.pitch = (unsigned int)msg.create_surface.w * 4;
		reply.surface_created.bpp = 32;
		reply.surface_created.red_mask_size = fi.red_mask_size;
		reply.surface_created.red_field_pos = fi.red_field_pos;
		reply.surface_created.green_mask_size = fi.green_mask_size;
		reply.surface_created.green_field_pos = fi.green_field_pos;
		reply.surface_created.blue_mask_size = fi.blue_mask_size;
		reply.surface_created.blue_field_pos = fi.blue_field_pos;

		if (wire_send(sfd, &reply, sizeof(reply), shmfd) != (int)sizeof(reply)) {
			printf("libguitest: FAIL - SURFACE_CREATED send failed\n");
			return 1;
		}
	}

	clientsurf.pixels = mmap(0, (unsigned long)msg.create_surface.w * 4 * (unsigned long)msg.create_surface.h,
	                          PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	if (clientsurf.pixels == MAP_FAILED) {
		printf("libguitest: FAIL - compositor-side mmap failed\n");
		return 1;
	}
	clientsurf.w = (unsigned int)msg.create_surface.w;
	clientsurf.h = (unsigned int)msg.create_surface.h;
	clientsurf.pitch = (unsigned int)msg.create_surface.w * 4;
	clientsurf.red_mask_size = fi.red_mask_size;
	clientsurf.red_field_pos = fi.red_field_pos;
	clientsurf.green_mask_size = fi.green_mask_size;
	clientsurf.green_field_pos = fi.green_field_pos;
	clientsurf.blue_mask_size = fi.blue_mask_size;
	clientsurf.blue_field_pos = fi.blue_field_pos;

	if (wire_recv(sfd, &msg, sizeof(msg), 0) <= 0 || msg.type != GUI_MSG_COMMIT) {
		printf("libguitest: FAIL - COMMIT not received\n");
		return 1;
	}

	gfx_blit(&fbsurf, BLIT_X, BLIT_Y, &clientsurf, 0, 0, (int)clientsurf.w, (int)clientsurf.h);

	waitpid(pid, &status, 0);
	if (status != 0) {
		printf("libguitest: FAIL - client reported failure\n");
		return 1;
	}

	printf("libguitest: PASS - libgui.so client + stub compositor + font rendering all confirmed working\n");
	return 0;
}
