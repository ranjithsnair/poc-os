/* socktest: throwaway diagnostic for AF_UNIX stream sockets +
 * SCM_RIGHTS fd-passing (kernel/socket.c, kernel/sysnet.c - GUI
 * roadmap phase 3, part of Wayland's own wire-protocol foundation:
 * every Wayland buffer handoff is exactly this mechanism). Proves
 * fd-passing lands a real, independently usable file descriptor, not
 * just a number: the child opens a file, writes a known marker string
 * into it, and passes *that* fd to the parent over the socket; the
 * parent reads the marker back through the *received* fd and confirms
 * it matches, rather than just checking that recvmsg() "succeeded".
 * Same dynamic Scrt1.o+libc.so PIE build as fbtest.c before it - see
 * this file's own Makefile rule.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#define MARKER "phase3-fd-passing-ok"
#define SOCKPATH "test.sock"

int
main(void)
{
	int lfd, sfd;
	struct sockaddr_un addr;
	pid_t pid;

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("socktest: socket failed\n");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCKPATH);

	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("socktest: bind failed\n");
		return 1;
	}
	if (listen(lfd, 4) < 0) {
		printf("socktest: listen failed\n");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("socktest: fork failed\n");
		return 1;
	}

	if (pid == 0) {
		int cfd, fd;
		struct msghdr msg;
		struct iovec iov;
		struct cmsghdr *cmsg;
		char cbuf[CMSG_SPACE(sizeof(int))];
		char payload[] = "hello";

		cfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (cfd < 0 || connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			printf("socktest: child connect failed\n");
			_exit(1);
		}

		fd = open("socktest.tmp", O_CREAT | O_RDWR, 0644);
		if (fd < 0) {
			printf("socktest: child open failed\n");
			_exit(1);
		}
		write(fd, MARKER, strlen(MARKER));
		lseek(fd, 0, SEEK_SET);

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
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
		msg.msg_controllen = CMSG_SPACE(sizeof(int));

		if (sendmsg(cfd, &msg, 0) < 0) {
			printf("socktest: sendmsg failed\n");
			_exit(1);
		}
		_exit(0);
	}

	sfd = accept(lfd, 0, 0);
	if (sfd < 0) {
		printf("socktest: accept failed\n");
		return 1;
	}

	{
		struct msghdr msg;
		struct iovec iov;
		struct cmsghdr *cmsg;
		char payload[16];
		char cbuf[CMSG_SPACE(sizeof(int))];
		int recvd_fd = -1;
		char rbuf[64];
		int rn, status;

		iov.iov_base = payload;
		iov.iov_len = sizeof(payload);
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);

		if (recvmsg(sfd, &msg, 0) < 0) {
			printf("socktest: recvmsg failed\n");
			return 1;
		}

		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(&recvd_fd, CMSG_DATA(cmsg), sizeof(int));

		if (recvd_fd < 0) {
			printf("socktest: FAIL - no fd received\n");
			return 1;
		}

		rn = read(recvd_fd, rbuf, sizeof(rbuf) - 1);
		if (rn < 0)
			rn = 0;
		rbuf[rn] = 0;

		waitpid(pid, &status, 0);

		if (strcmp(rbuf, MARKER) == 0)
			printf("socktest: PASS - received fd content = \"%s\"\n", rbuf);
		else
			printf("socktest: FAIL - received fd content = \"%s\"\n", rbuf);
	}

	return 0;
}
