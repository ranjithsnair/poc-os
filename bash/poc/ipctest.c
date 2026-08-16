/* ipctest: throwaway diagnostic verifying kernel/socket.c, shm.c,
 * epoll.c (GUI roadmap phase 3's Wayland IPC primitives) still work
 * after being restored from commit ad8d47e (they were deleted in
 * e149182 and reinstated for GUI roadmap phase 6) - the exact
 * end-to-end path a real compositor needs: a client shm_create()s a
 * buffer, writes a pixel-pattern marker into it, then passes that fd
 * to the "compositor" over an AF_UNIX socket via SCM_RIGHTS, and the
 * receiving side mmaps the *received* fd and confirms it sees the
 * same physical pages - not a copy. epoll_wait() is used (instead of
 * a blocking accept()/recvmsg()) specifically to also prove epoll
 * itself still fires readiness events on both a listening socket and
 * a connected one, since Phase 8's compositor loop depends on that.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

extern long syscall(long, ...);

#define SHM_SIZE 4096
#define MARKER 0xC0FFEE42u
#define SOCKPATH "ipctest.sock"

static int
wait_readable(int epfd, int fd)
{
	struct epoll_event ev;
	int n;

	n = epoll_wait(epfd, &ev, 1, 5000);
	if (n <= 0)
		return -1;
	return ev.data.fd == fd ? 0 : -1;
}

int
main(void)
{
	int lfd, epfd, sfd;
	struct sockaddr_un addr;
	struct epoll_event ev;
	pid_t pid;

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("ipctest: FAIL - socket failed\n");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCKPATH);

	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("ipctest: FAIL - bind failed\n");
		return 1;
	}
	if (listen(lfd, 4) < 0) {
		printf("ipctest: FAIL - listen failed\n");
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("ipctest: FAIL - epoll_create1 failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = lfd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
		printf("ipctest: FAIL - epoll_ctl(listen) failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("ipctest: FAIL - fork failed\n");
		return 1;
	}

	if (pid == 0) {
		int cfd, shmfd;
		unsigned int *p;
		struct msghdr msg;
		struct iovec iov;
		struct cmsghdr *cmsg;
		char cbuf[CMSG_SPACE(sizeof(int))];
		char payload[] = "buf";

		shmfd = syscall(SYS_shm_create, SHM_SIZE);
		if (shmfd < 0) {
			printf("ipctest: FAIL - child shm_create failed\n");
			_exit(1);
		}
		p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
		if (p == MAP_FAILED) {
			printf("ipctest: FAIL - child mmap failed\n");
			_exit(1);
		}
		*p = MARKER;

		cfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (cfd < 0 || connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			printf("ipctest: FAIL - child connect failed\n");
			_exit(1);
		}

		iov.iov_base = payload;
		iov.iov_len = sizeof(payload);
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), &shmfd, sizeof(int));
		msg.msg_controllen = CMSG_SPACE(sizeof(int));

		if (sendmsg(cfd, &msg, 0) < 0) {
			printf("ipctest: FAIL - child sendmsg failed\n");
			_exit(1);
		}
		_exit(0);
	}

	if (wait_readable(epfd, lfd) < 0) {
		printf("ipctest: FAIL - epoll never saw listening socket readable\n");
		return 1;
	}

	sfd = accept(lfd, 0, 0);
	if (sfd < 0) {
		printf("ipctest: FAIL - accept failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = sfd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) < 0) {
		printf("ipctest: FAIL - epoll_ctl(conn) failed\n");
		return 1;
	}
	if (wait_readable(epfd, sfd) < 0) {
		printf("ipctest: FAIL - epoll never saw connection readable\n");
		return 1;
	}

	{
		struct msghdr msg;
		struct iovec iov;
		struct cmsghdr *cmsg;
		char payload[16];
		char cbuf[CMSG_SPACE(sizeof(int))];
		int recvd_fd = -1;
		unsigned int *p;
		int status;

		iov.iov_base = payload;
		iov.iov_len = sizeof(payload);
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);

		if (recvmsg(sfd, &msg, 0) < 0) {
			printf("ipctest: FAIL - recvmsg failed\n");
			return 1;
		}

		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(&recvd_fd, CMSG_DATA(cmsg), sizeof(int));

		if (recvd_fd < 0) {
			printf("ipctest: FAIL - no fd received over SCM_RIGHTS\n");
			return 1;
		}

		p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, recvd_fd, 0);
		if (p == MAP_FAILED) {
			printf("ipctest: FAIL - mmap of received fd failed\n");
			return 1;
		}

		waitpid(pid, &status, 0);

		if (*p == MARKER)
			printf("ipctest: PASS - socket+SCM_RIGHTS+shm+epoll all confirmed working\n");
		else
			printf("ipctest: FAIL - saw 0x%x, expected 0x%x\n", *p, MARKER);
	}

	return 0;
}
