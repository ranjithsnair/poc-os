/* wire.h: minimal real Wayland wire-protocol encoding helpers, shared
 * by bash/poc/wltest.c's compositor_main()/client_main() (GUI roadmap
 * phase 4 - see /Users/ranjith/.claude/plans/structured-stargazing-pixel.md
 * for the full design and the opcode table this was verified against).
 * Every message here is small enough for kernel/socket.h's SOCKMSGSIZE
 * (512 bytes) and carries at most one fd, well inside MAXFDMSG (4), so
 * one Wayland message maps to exactly one sendmsg()/recvmsg() call -
 * no framing beyond the wire format's own 8-byte header is needed.
 * static inline throughout - a header-only helper, not a separate
 * compile unit, included by wltest.c only.
 */
#ifndef WIRE_H
#define WIRE_H

#include <string.h>
#include <sys/socket.h>

/* id:uint32 + packed (size<<16 | opcode):uint32 - both native-endian
 * on the wire, no byteswapping needed on x86_64 (see wire.h's own
 * top comment / the plan doc's wire-format section for why).
 */
static inline void
put_header(unsigned char *buf, unsigned int id, unsigned short opcode, unsigned short size)
{
	*(unsigned int *)buf = id;
	*(unsigned int *)(buf + 4) = ((unsigned int)size << 16) | opcode;
}

static inline void
put_uint(unsigned char *buf, unsigned int off, unsigned int v)
{
	*(unsigned int *)(buf + off) = v;
}

static inline unsigned int
get_uint(const unsigned char *buf, unsigned int off)
{
	return *(const unsigned int *)(buf + off);
}

/* Wire string: uint32 byte-length (including the NUL) then that many
 * bytes, zero-padded to a 4-byte boundary. Returns the total bytes
 * written (4 + padded length) so callers can advance their offset. */
static inline unsigned int
put_string(unsigned char *buf, unsigned int off, const char *s)
{
	unsigned int len = strlen(s) + 1;
	unsigned int padded = (len + 3) & ~3u;
	unsigned int i;

	put_uint(buf, off, len);
	memcpy(buf + off + 4, s, len);
	for (i = len; i < padded; i++)
		buf[off + 4 + i] = 0;
	return 4 + padded;
}

/* Reverse of put_string() - reads into out (truncating to outsz),
 * returns the total bytes consumed (4 + padded length). */
static inline unsigned int
get_string(const unsigned char *buf, unsigned int off, char *out, unsigned int outsz)
{
	unsigned int len = get_uint(buf, off);
	unsigned int padded = (len + 3) & ~3u;
	unsigned int n = len < outsz ? len : outsz - 1;

	memcpy(out, buf + off + 4, n);
	out[n] = 0;
	return 4 + padded;
}

/* Sends one already-built wire message, with an optional single fd
 * riding as SCM_RIGHTS ancillary data (fd < 0: none - matches every
 * request here except wl_shm.create_pool). */
static inline int
wl_send(int sock, unsigned char *buf, int len, int fd)
{
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	char cbuf[CMSG_SPACE(sizeof(int))];

	iov.iov_base = buf;
	iov.iov_len = len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (fd >= 0) {
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
		msg.msg_controllen = CMSG_SPACE(sizeof(int));
	}

	return sendmsg(sock, &msg, 0);
}

/* Receives one wire message into buf (capacity bufsz); *fd is set to
 * a received fd, or -1 if none rode along. Returns the message
 * length, or a negative value on error. */
static inline int
wl_recv(int sock, unsigned char *buf, int bufsz, int *fd)
{
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	char cbuf[CMSG_SPACE(sizeof(int))];
	int n;

	iov.iov_base = buf;
	iov.iov_len = bufsz;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	*fd = -1;
	n = recvmsg(sock, &msg, 0);
	if (n >= 0) {
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
	}
	return n;
}

#endif
