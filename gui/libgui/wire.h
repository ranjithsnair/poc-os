// wire: sendmsg()/recvmsg() helpers shared by libgui's client.c and
// gui/compositor.c. kernel/socket.c's FD_SOCK type is handled *only*
// by sockrecv()/socksend() (kernel/sysnet.c's sys_sendmsg/recvmsg) -
// plain read()/write() on a socket fd falls through fileread()/
// filewrite() (kernel/file.c) to their trailing panic("fileread")/
// panic("filewrite"), since neither function has an FD_SOCK case.
// Every socket message in this protocol MUST go through these
// wrappers, never read()/write()/send()/recv() (poc-os has no
// SYS_sendto/recvfrom for the latter two to lower to anyway).
#ifndef GUI_WIRE_H
#define GUI_WIRE_H

// Sends exactly one message (data/len), optionally with one fd riding
// along via SCM_RIGHTS (pass fd = -1 for none). Returns len on
// success, -1 on failure.
int wire_send(int sockfd, void *data, int len, int fd);

// Receives exactly one message into data (up to len bytes). If
// recv_fd is non-NULL, an SCM_RIGHTS fd is decoded into *recv_fd (set
// to -1 if none arrived). Returns the byte count (0 = peer closed,
// matching sockrecv()'s own EOF convention), -1 on failure.
int wire_recv(int sockfd, void *data, int len, int *recv_fd);

#endif
