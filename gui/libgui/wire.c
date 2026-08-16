#include <string.h>
#include <sys/socket.h>
#include "wire.h"

int
wire_send(int sockfd, void *data, int len, int fd)
{
  struct msghdr msg;
  struct iovec iov;
  char cbuf[CMSG_SPACE(sizeof(int))];

  iov.iov_base = data;
  iov.iov_len = len;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  if (fd >= 0) {
    struct cmsghdr *cmsg;

    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    msg.msg_controllen = CMSG_SPACE(sizeof(int));
  }

  return sendmsg(sockfd, &msg, 0);
}

int
wire_recv(int sockfd, void *data, int len, int *recv_fd)
{
  struct msghdr msg;
  struct iovec iov;
  char cbuf[CMSG_SPACE(sizeof(int))];
  int n;

  iov.iov_base = data;
  iov.iov_len = len;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);

  n = recvmsg(sockfd, &msg, 0);
  if (n < 0)
    return -1;

  if (recv_fd) {
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    *recv_fd = -1;
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
      memcpy(recv_fd, CMSG_DATA(cmsg), sizeof(int));
  }
  return n;
}
