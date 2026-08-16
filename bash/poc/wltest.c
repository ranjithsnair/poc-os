/* wltest: throwaway diagnostic for a minimal, genuinely wire-correct
 * Wayland handshake (GUI roadmap phase 4 - see the plan doc at
 * /Users/ranjith/.claude/plans/structured-stargazing-pixel.md for the
 * verified opcode table and full design). Not a link against upstream
 * libwayland - that needs wayland-scanner/protocol XML and a meson
 * build this repo has neither of - but real spec-correct bytes on the
 * wire, built entirely on phases 2-3's own primitives: an AF_UNIX
 * socket (kernel/socket.c), SCM_RIGHTS fd-passing, shared memory
 * (kernel/shm.c), and the VBE framebuffer (kernel/vbe.c).
 *
 * fork()s into a compositor stub and a client, exactly the way
 * socktest.c forks into a listener and a connector. The compositor
 * binds "wayland-0", accepts one client, and dispatches its handful of
 * requests by (object id's recorded type, opcode) into a small fixed
 * table - see struct obj below. The client does the real get_registry
 * -> bind -> create_surface -> create_pool(+fd) -> create_buffer ->
 * attach -> commit sequence, drawing the same five-band color-bar
 * pattern fbtest.c already uses into its shm pool. On commit, the
 * compositor blits those bars onto the real framebuffer at (50,50) -
 * a small placed rectangle, not a full-screen fill, so a screendump
 * clearly shows the compositor actually placed the client's surface
 * rather than just re-proving fbtest.c's own direct-mmap path.
 *
 * Same dynamic Scrt1.o+libc.so PIE build as fbtest.c/socktest.c
 * before it - see this file's own Makefile rule.
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

#define SOCKPATH "wayland-0"

enum { T_NONE, T_REGISTRY, T_COMPOSITOR, T_SHM, T_SURFACE, T_SHM_POOL, T_BUFFER };
#define MAXOBJ 16

struct obj {
	int type;
	void *pool_base;
	unsigned int pool_size;
	int buf_pool, buf_offset, buf_width, buf_height, buf_stride, buf_format;
	int pending_buffer;
};

static struct obj objs[MAXOBJ];

static void
compositor_main(int readyfd)
{
	int lfd, cfd, fbfd;
	struct sockaddr_un addr;
	unsigned char buf[512], ev[64];
	int fd, n, off;
	struct fb_info fi;
	unsigned char *fb;

	memset(objs, 0, sizeof(objs));

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("wltest: compositor socket failed\n");
		return;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCKPATH);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("wltest: compositor bind failed\n");
		return;
	}
	if (listen(lfd, 1) < 0) {
		printf("wltest: compositor listen failed\n");
		return;
	}
	printf("wltest: compositor listening on \"%s\"\n", SOCKPATH);
	write(readyfd, "x", 1);

	cfd = accept(lfd, 0, 0);
	if (cfd < 0) {
		printf("wltest: compositor accept failed\n");
		return;
	}
	printf("wltest: compositor accepted client\n");

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("wltest: compositor no usable framebuffer\n");
		return;
	}
	fb = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == MAP_FAILED) {
		printf("wltest: compositor framebuffer mmap failed\n");
		return;
	}

	for (;;) {
		unsigned int id, opcode;

		n = wl_recv(cfd, buf, sizeof(buf), &fd);
		if (n <= 0) {
			printf("wltest: compositor client disconnected\n");
			break;
		}
		id = get_uint(buf, 0);
		opcode = get_uint(buf, 4) & 0xffff;

		if (id == 1 && opcode == 1) {
			/* wl_display.get_registry(new_id registry) */
			unsigned int reg = get_uint(buf, 8);

			objs[reg].type = T_REGISTRY;
			printf("wltest: compositor: get_registry -> id %u\n", reg);

			off = 12;
			put_uint(ev, 8, 1);
			off += put_string(ev, off, "wl_compositor");
			put_uint(ev, off, 1);
			off += 4;
			put_header(ev, reg, 0, off);
			wl_send(cfd, ev, off, -1);

			off = 12;
			put_uint(ev, 8, 2);
			off += put_string(ev, off, "wl_shm");
			put_uint(ev, off, 1);
			off += 4;
			put_header(ev, reg, 0, off);
			wl_send(cfd, ev, off, -1);
			printf("wltest: compositor: sent wl_registry.global x2\n");

		} else if (id < MAXOBJ && objs[id].type == T_REGISTRY && opcode == 0) {
			/* wl_registry.bind(name, interface, version, id) */
			char iface[32];
			unsigned int newid;

			off = 12;
			off += get_string(buf, off, iface, sizeof(iface));
			off += 4;  /* version, unused */
			newid = get_uint(buf, off);

			if (strcmp(iface, "wl_compositor") == 0)
				objs[newid].type = T_COMPOSITOR;
			else if (strcmp(iface, "wl_shm") == 0)
				objs[newid].type = T_SHM;
			printf("wltest: compositor: bound %s -> id %u\n", iface, newid);

		} else if (id < MAXOBJ && objs[id].type == T_COMPOSITOR && opcode == 0) {
			/* wl_compositor.create_surface(new_id surface) */
			unsigned int newid = get_uint(buf, 8);

			objs[newid].type = T_SURFACE;
			objs[newid].pending_buffer = 0;
			printf("wltest: compositor: created surface id %u\n", newid);

		} else if (id < MAXOBJ && objs[id].type == T_SHM && opcode == 0) {
			/* wl_shm.create_pool(new_id pool, fd, int size) */
			unsigned int newid = get_uint(buf, 8);
			int size = (int)get_uint(buf, 12);
			void *base;

			if (fd < 0) {
				printf("wltest: compositor: create_pool missing fd\n");
				break;
			}
			base = mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
			if (base == MAP_FAILED) {
				printf("wltest: compositor: pool mmap failed\n");
				break;
			}
			objs[newid].type = T_SHM_POOL;
			objs[newid].pool_base = base;
			objs[newid].pool_size = size;
			printf("wltest: compositor: created pool id %u (%d bytes, via SCM_RIGHTS fd)\n",
			       newid, size);

		} else if (id < MAXOBJ && objs[id].type == T_SHM_POOL && opcode == 0) {
			/* wl_shm_pool.create_buffer(new_id, offset, w, h, stride, format) */
			unsigned int newid = get_uint(buf, 8);

			objs[newid].type = T_BUFFER;
			objs[newid].buf_pool = id;
			objs[newid].buf_offset = (int)get_uint(buf, 12);
			objs[newid].buf_width = (int)get_uint(buf, 16);
			objs[newid].buf_height = (int)get_uint(buf, 20);
			objs[newid].buf_stride = (int)get_uint(buf, 24);
			objs[newid].buf_format = (int)get_uint(buf, 28);
			printf("wltest: compositor: created buffer id %u (%dx%d)\n",
			       newid, objs[newid].buf_width, objs[newid].buf_height);

		} else if (id < MAXOBJ && objs[id].type == T_SURFACE && opcode == 1) {
			/* wl_surface.attach(buffer, x, y) */
			objs[id].pending_buffer = (int)get_uint(buf, 8);
			printf("wltest: compositor: surface %u attach buffer %u\n",
			       id, objs[id].pending_buffer);

		} else if (id < MAXOBJ && objs[id].type == T_SURFACE && opcode == 6) {
			/* wl_surface.commit() */
			struct obj *bo = &objs[objs[id].pending_buffer];
			struct obj *po = &objs[bo->buf_pool];
			unsigned char *src = (unsigned char *)po->pool_base + bo->buf_offset;
			int x0 = 50, y0 = 50, x, y;

			printf("wltest: compositor: surface %u commit, blitting %dx%d at (%d,%d)\n",
			       id, bo->buf_width, bo->buf_height, x0, y0);

			for (y = 0; y < bo->buf_height; y++) {
				unsigned char *srow = src + (unsigned long)y * bo->buf_stride;
				unsigned char *drow = fb + (unsigned long)(y0 + y) * fi.pitch;

				for (x = 0; x < bo->buf_width; x++) {
					unsigned int px = *(unsigned int *)(srow + x * 4);
					unsigned int r = (px >> 16) & 0xff;
					unsigned int g = (px >> 8) & 0xff;
					unsigned int b = px & 0xff;
					unsigned int outpx = (r << fi.red_field_pos) |
					                     (g << fi.green_field_pos) |
					                     (b << fi.blue_field_pos);

					*(unsigned int *)(drow + (x0 + x) * 4) = outpx;
				}
			}
			printf("wltest: compositor: PASS - blitted client surface to framebuffer\n");
			return;
		} else {
			printf("wltest: compositor: unknown message id=%u opcode=%u\n", id, opcode);
		}
	}
}

static void
client_main(void)
{
	int sock, shmfd, fd, n, off;
	struct sockaddr_un addr;
	unsigned char buf[512];
	unsigned int compositor_name = 0, shm_name = 0;
	int seen_compositor = 0, seen_shm = 0;
	unsigned int registry_id = 2, compositor_id = 3, shm_id = 4;
	unsigned int surface_id = 5, pool_id = 6, buffer_id = 7;
	/* 200x150x4 = 120000 bytes, comfortably under include/shm.h's
	 * SHM_MAXPAGES cap (64 pages = 256KB) - still a clearly visible
	 * placed rectangle in a screendump, just smaller than an earlier
	 * 400x300 attempt that exceeded that cap and failed shm_create(). */
	int width = 200, height = 150, stride;
	unsigned int poolsize, colors[5];
	unsigned char *pix;
	int x, y;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		printf("wltest: client socket failed\n");
		return;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCKPATH);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("wltest: client connect failed\n");
		return;
	}
	printf("wltest: client connected\n");

	/* wl_display.get_registry(new_id registry) - opcode 1 */
	put_header(buf, 1, 1, 12);
	put_uint(buf, 8, registry_id);
	wl_send(sock, buf, 12, -1);

	while (!seen_compositor || !seen_shm) {
		char iface[32];

		n = wl_recv(sock, buf, sizeof(buf), &fd);
		if (n <= 0) {
			printf("wltest: client recv global failed\n");
			return;
		}
		off = 12;
		off += get_string(buf, off, iface, sizeof(iface));
		if (strcmp(iface, "wl_compositor") == 0) {
			compositor_name = get_uint(buf, 8);
			seen_compositor = 1;
		} else if (strcmp(iface, "wl_shm") == 0) {
			shm_name = get_uint(buf, 8);
			seen_shm = 1;
		}
	}
	printf("wltest: client saw both globals (wl_compositor name=%u, wl_shm name=%u)\n",
	       compositor_name, shm_name);

	/* wl_registry.bind(name, interface, version, id) - opcode 0 */
	off = 12;
	put_uint(buf, 8, compositor_name);
	off += put_string(buf, off, "wl_compositor");
	put_uint(buf, off, 1);
	off += 4;
	put_uint(buf, off, compositor_id);
	off += 4;
	put_header(buf, registry_id, 0, off);
	wl_send(sock, buf, off, -1);

	off = 12;
	put_uint(buf, 8, shm_name);
	off += put_string(buf, off, "wl_shm");
	put_uint(buf, off, 1);
	off += 4;
	put_uint(buf, off, shm_id);
	off += 4;
	put_header(buf, registry_id, 0, off);
	wl_send(sock, buf, off, -1);
	printf("wltest: client: bound wl_compositor=%u wl_shm=%u\n", compositor_id, shm_id);

	/* wl_compositor.create_surface(new_id surface) - opcode 0 */
	put_header(buf, compositor_id, 0, 12);
	put_uint(buf, 8, surface_id);
	wl_send(sock, buf, 12, -1);
	printf("wltest: client: created surface id %u\n", surface_id);

	stride = width * 4;
	poolsize = (unsigned int)(stride * height);
	shmfd = syscall(SYS_shm_create, poolsize);
	if (shmfd < 0) {
		printf("wltest: client shm_create failed\n");
		return;
	}
	pix = mmap(0, poolsize, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	if (pix == MAP_FAILED) {
		printf("wltest: client shm mmap failed\n");
		return;
	}
	colors[0] = 0xFF0000; colors[1] = 0x00FF00; colors[2] = 0x0000FF;
	colors[3] = 0xFFFFFF; colors[4] = 0x000000;
	for (y = 0; y < height; y++) {
		unsigned int band = ((unsigned int)y * 5) / (unsigned int)height;
		unsigned int *row = (unsigned int *)(pix + (unsigned long)y * stride);

		for (x = 0; x < width; x++)
			row[x] = colors[band];
	}
	printf("wltest: client: drew color bars into shm pool (%dx%d)\n", width, height);

	/* wl_shm.create_pool(new_id pool, fd, int size) - opcode 0 */
	put_header(buf, shm_id, 0, 16);
	put_uint(buf, 8, pool_id);
	put_uint(buf, 12, poolsize);
	wl_send(sock, buf, 16, shmfd);

	/* wl_shm_pool.create_buffer(new_id, offset, w, h, stride, format=XRGB8888) - opcode 0 */
	put_header(buf, pool_id, 0, 32);
	put_uint(buf, 8, buffer_id);
	put_uint(buf, 12, 0);
	put_uint(buf, 16, (unsigned int)width);
	put_uint(buf, 20, (unsigned int)height);
	put_uint(buf, 24, (unsigned int)stride);
	put_uint(buf, 28, 1);
	wl_send(sock, buf, 32, -1);

	/* wl_surface.attach(buffer, x, y) - opcode 1 */
	put_header(buf, surface_id, 1, 20);
	put_uint(buf, 8, buffer_id);
	put_uint(buf, 12, 0);
	put_uint(buf, 16, 0);
	wl_send(sock, buf, 20, -1);

	/* wl_surface.commit() - opcode 6 */
	put_header(buf, surface_id, 6, 8);
	wl_send(sock, buf, 8, -1);

	printf("wltest: client: PASS - sent create_pool/create_buffer/attach/commit\n");
}

int
main(void)
{
	int syncfd[2];
	pid_t pid;
	int status;

	if (pipe(syncfd) < 0) {
		printf("wltest: pipe failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("wltest: fork failed\n");
		return 1;
	}
	if (pid == 0) {
		close(syncfd[0]);
		compositor_main(syncfd[1]);
		_exit(0);
	}

	close(syncfd[1]);
	{
		char c;
		read(syncfd[0], &c, 1);  /* wait for compositor's bind()+listen() */
	}
	client_main();
	waitpid(pid, &status, 0);
	return 0;
}
