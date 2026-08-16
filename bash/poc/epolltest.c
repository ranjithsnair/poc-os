/* epolltest: throwaway diagnostic for epoll (kernel/epoll.c - GUI
 * roadmap phase 3, both a real Wayland compositor and client build
 * their event loop around this). Proves readiness detection actually
 * reflects real state, not just "always returns ready": a pipe read
 * end is registered, epoll_wait() with nothing written yet must report
 * 0 (timeout), then after this same process writes to the pipe a
 * second epoll_wait() call must report it ready. Same dynamic
 * Scrt1.o+libc.so PIE build as fbtest.c before it.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>

int
main(void)
{
	int pfd[2], epfd, n;
	struct epoll_event ev, out[4];

	if (pipe(pfd) < 0) {
		printf("epolltest: pipe failed\n");
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("epolltest: epoll_create1 failed\n");
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = pfd[0];
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev) < 0) {
		printf("epolltest: epoll_ctl ADD failed\n");
		return 1;
	}

	n = epoll_wait(epfd, out, 4, 300);
	if (n != 0) {
		printf("epolltest: FAIL - expected timeout (0 ready), got %d\n", n);
		return 1;
	}
	printf("epolltest: timeout-before-write ok (0 ready)\n");

	write(pfd[1], "x", 1);

	n = epoll_wait(epfd, out, 4, 2000);
	if (n == 1 && out[0].data.fd == pfd[0] && (out[0].events & EPOLLIN))
		printf("epolltest: PASS - fd reported ready after write\n");
	else {
		printf("epolltest: FAIL - got n=%d\n", n);
		return 1;
	}

	return 0;
}
