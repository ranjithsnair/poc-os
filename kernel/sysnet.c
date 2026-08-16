// AF_UNIX socket system calls (GUI roadmap phase 3) - thin argument-
// parsing glue around kernel/socket.c's mechanism, the same
// relationship kernel/sysfile.c's sys_pipe() has to kernel/pipe.c.
//
// struct msghdr/struct cmsghdr aren't available to kernel code (no
// musl headers here) - offsets below are hand-derived from
// musl/include/sys/socket.h for this build's LP64/little-endian
// layout, the same "decode fixed byte offsets by hand" approach
// sysfile.c's sys_writev()/sys_readv() already use for struct iovec.
//
//   struct msghdr {           struct cmsghdr {
//     void      *msg_name;  0    socklen_t cmsg_len;   0
//     socklen_t  msg_namelen; 8  int       __pad1;      4
//     (4 bytes compiler pad)     int       cmsg_level;  8
//     struct iovec *msg_iov; 16  int       cmsg_type;  12
//     int   msg_iovlen;      24  // data follows at offset 16
//     int   __pad1;          28
//     void *msg_control;     32
//     socklen_t msg_controllen; 40
//     int __pad2;            44
//     int msg_flags;         48
//   };                          // sizeof == 56

#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "socket.h"

#define MSG_NAME    0
#define MSG_NAMELEN 8
#define MSG_IOV     16
#define MSG_IOVLEN  24
#define MSG_CONTROL 32
#define MSG_CTLLEN  40
#define MSG_FLAGS   48

#define CMSG_HDRLEN 16   // cmsg_len(4)+pad(4)+cmsg_level(4)+cmsg_type(4)

#define AF_UNIX_ 1
#define SOCK_STREAM_ 1
#define SOCK_TYPEMASK 0xFF  // musl ORs SOCK_CLOEXEC/SOCK_NONBLOCK into
                             // the high bits of type - ignore them,
                             // this kernel has no close-on-exec/
                             // non-blocking-fd concept to honor anyway
#define SOL_SOCKET_ 1
#define SCM_RIGHTS_ 1
#define MSG_CTRUNC_ 0x0008

int
sys_socket(void)
{
  int domain, type, protocol;
  struct file *f;
  int fd;

  if(argint(0, &domain) < 0 || argint(1, &type) < 0 || argint(2, &protocol) < 0)
    return -1;
  if(domain != AF_UNIX_ || (type & SOCK_TYPEMASK) != SOCK_STREAM_)
    return -1;
  if((f = sockcreate()) == 0)
    return -1;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -1;
  }
  return fd;
}

// Reads a struct sockaddr_un's sun_path (offset 2, after the 2-byte
// sa_family_t) out of user memory into a kernel-local buffer.
static int
fetchsunpath(int addr, int len, char *path)
{
  int i, n;

  if(len < 2 || (uint)addr >= myproc()->sz || (uint)addr + len > myproc()->sz)
    return -1;
  n = len - 2;
  if(n >= SUNPATHMAX)
    n = SUNPATHMAX - 1;
  for(i = 0; i < n; i++){
    char c = *((char*)(uintp)(uint)addr + 2 + i);
    path[i] = c;
    if(c == 0)
      break;
  }
  path[i < n ? i : n] = 0;
  return 0;
}

int
sys_bind(void)
{
  struct file *f;
  int addr, len;
  char path[SUNPATHMAX];

  if(argfd(0, 0, &f) < 0 || argint(1, &addr) < 0 || argint(2, &len) < 0)
    return -1;
  if(f->type != FD_SOCK)
    return -1;
  if(fetchsunpath(addr, len, path) < 0 || path[0] == 0)
    return -1;
  return sockbind(f->sock, path);
}

int
sys_listen(void)
{
  struct file *f;
  int backlog;

  if(argfd(0, 0, &f) < 0 || argint(1, &backlog) < 0)
    return -1;
  if(f->type != FD_SOCK)
    return -1;
  return socklisten(f->sock);
}

int
sys_connect(void)
{
  struct file *f;
  int addr, len;
  char path[SUNPATHMAX];

  if(argfd(0, 0, &f) < 0 || argint(1, &addr) < 0 || argint(2, &len) < 0)
    return -1;
  if(f->type != FD_SOCK)
    return -1;
  if(fetchsunpath(addr, len, path) < 0 || path[0] == 0)
    return -1;
  return sockconnect(f->sock, path);
}

int
sys_accept(void)
{
  struct file *lf, *nf;
  struct socket *ns;
  int fd;

  if(argfd(0, 0, &lf) < 0)
    return -1;
  if(lf->type != FD_SOCK)
    return -1;
  if((ns = sockaccept(lf->sock)) == 0)
    return -1;
  if((nf = filealloc()) == 0){
    ns->inuse = 0;
    return -1;
  }
  nf->type = FD_SOCK;
  nf->readable = 1;
  nf->writable = 1;
  nf->sock = ns;
  if((fd = fdalloc(nf)) < 0){
    fileclose(nf);
    return -1;
  }
  return fd;
}

int
sys_socketpair(void)
{
  int domain, type, protocol, addr;
  struct socket *s0, *s1;
  struct file *f0, *f1;
  int fd0, fd1;

  if(argint(0, &domain) < 0 || argint(1, &type) < 0 ||
     argint(2, &protocol) < 0 || argint(3, &addr) < 0)
    return -1;
  if(domain != AF_UNIX_ || (type & SOCK_TYPEMASK) != SOCK_STREAM_)
    return -1;
  if((uint)addr >= myproc()->sz || (uint)addr + 8 > myproc()->sz)
    return -1;
  if(sockpair(&s0, &s1) < 0)
    return -1;
  if((f0 = filealloc()) == 0 || (f1 = filealloc()) == 0){
    if(f0) fileclose(f0);
    s0->inuse = 0;
    s1->inuse = 0;
    return -1;
  }
  f0->type = FD_SOCK; f0->readable = 1; f0->writable = 1; f0->sock = s0;
  f1->type = FD_SOCK; f1->readable = 1; f1->writable = 1; f1->sock = s1;
  if((fd0 = fdalloc(f0)) < 0 || (fd1 = fdalloc(f1)) < 0){
    if(fd0 >= 0) myproc()->ofile[fd0] = 0;
    fileclose(f0);
    fileclose(f1);
    return -1;
  }
  *(int*)((char*)(uintp)(uint)addr) = fd0;
  *(int*)((char*)(uintp)(uint)addr + 4) = fd1;
  return 0;
}

// argfd() (kernel/sysfile.c) reads its fd from a syscall argument slot
// - sendmsg's fds come from inside a control-message buffer instead,
// so this does argfd()'s ofile[] lookup without the argument-fetch
// half.
static int
argfd_check(int fd, struct file **pf)
{
  if(fd < 0 || fd >= NOFILE || (*pf = myproc()->ofile[fd]) == 0)
    return -1;
  return 0;
}

int
sys_sendmsg(void)
{
  struct file *f;
  int msgp, flags;
  int iovp, iovlen, ctlp, ctllen;
  char buf[SOCKMSGSIZE];
  struct file *fds[MAXFDMSG];
  int nfds, i, off, base, len, n;

  if(argfd(0, 0, &f) < 0 || argint(1, &msgp) < 0 || argint(2, &flags) < 0)
    return -1;
  if(f->type != FD_SOCK)
    return -1;
  if(fetchint(msgp + MSG_IOV, &iovp) < 0 || fetchint(msgp + MSG_IOVLEN, &iovlen) < 0 ||
     fetchint(msgp + MSG_CONTROL, &ctlp) < 0 || fetchint(msgp + MSG_CTLLEN, &ctllen) < 0)
    return -1;
  if(iovlen < 0 || iovlen > 16)
    return -1;

  off = 0;
  for(i = 0; i < iovlen; i++){
    if(fetchint(iovp + 16*i, &base) < 0 || fetchint(iovp + 16*i + 8, &len) < 0)
      return -1;
    if(len == 0)
      continue;
    if(len < 0 || off + len > SOCKMSGSIZE)
      return -1;
    if((uint)base >= myproc()->sz || (uint)base + len > myproc()->sz)
      return -1;
    memmove(buf + off, (char*)(uintp)(uint)base, len);
    off += len;
  }

  nfds = 0;
  if(ctlp != 0 && ctllen >= CMSG_HDRLEN){
    int clen, clevel, ctype, ndata;

    if(fetchint(ctlp, &clen) < 0 || fetchint(ctlp + 8, &clevel) < 0 ||
       fetchint(ctlp + 12, &ctype) < 0)
      return -1;
    if(clevel == SOL_SOCKET_ && ctype == SCM_RIGHTS_){
      ndata = clen - CMSG_HDRLEN;
      if(ndata < 0)
        ndata = 0;
      nfds = ndata / 4;
      if(nfds > MAXFDMSG)
        nfds = MAXFDMSG;
      for(i = 0; i < nfds; i++){
        int ufd;
        struct file *uf;

        if(fetchint(ctlp + CMSG_HDRLEN + 4*i, &ufd) < 0)
          return -1;
        if(argfd_check(ufd, &uf) < 0)
          return -1;
        fds[i] = filedup(uf);
      }
    }
  }

  n = socksend(f->sock, buf, off, fds, nfds);
  if(n < 0){
    for(i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return -1;
  }
  return n;
}

int
sys_recvmsg(void)
{
  struct file *f;
  int msgp, flags;
  int iovp, iovlen, ctlp, ctllen;
  char buf[SOCKMSGSIZE];
  struct file *fds[MAXFDMSG];
  int nfds, i, off, base, len, remain, n;

  if(argfd(0, 0, &f) < 0 || argint(1, &msgp) < 0 || argint(2, &flags) < 0)
    return -1;
  if(f->type != FD_SOCK)
    return -1;
  if(fetchint(msgp + MSG_IOV, &iovp) < 0 || fetchint(msgp + MSG_IOVLEN, &iovlen) < 0 ||
     fetchint(msgp + MSG_CONTROL, &ctlp) < 0 || fetchint(msgp + MSG_CTLLEN, &ctllen) < 0)
    return -1;
  if(iovlen < 0 || iovlen > 16)
    return -1;

  n = sockrecv(f->sock, buf, SOCKMSGSIZE, fds, &nfds, MAXFDMSG);
  if(n < 0)
    return -1;

  off = 0;
  remain = n;
  for(i = 0; i < iovlen && remain > 0; i++){
    if(fetchint(iovp + 16*i, &base) < 0 || fetchint(iovp + 16*i + 8, &len) < 0)
      break;
    if(len <= 0)
      continue;
    if(len > remain)
      len = remain;
    if((uint)base >= myproc()->sz || (uint)base + len > myproc()->sz)
      break;
    memmove((char*)(uintp)(uint)base, buf + off, len);
    off += len;
    remain -= len;
  }

  if(nfds > 0 && ctlp != 0 && (uint)ctlp < myproc()->sz &&
     (uint)ctlp + CMSG_HDRLEN + nfds*4 <= myproc()->sz &&
     (uint)ctllen >= (uint)(CMSG_HDRLEN + nfds*4)){
    int *cm = (int*)(uintp)(uint)ctlp;
    int installed = 0;

    for(i = 0; i < nfds; i++){
      int newfd = fdalloc(fds[i]);
      if(newfd < 0){
        fileclose(fds[i]);
        continue;
      }
      cm[4 + installed] = newfd;
      installed++;
    }
    cm[0] = CMSG_HDRLEN + installed*4;  // cmsg_len
    cm[1] = 0;                           // __pad1
    cm[2] = SOL_SOCKET_;                 // cmsg_level
    cm[3] = SCM_RIGHTS_;                 // cmsg_type
    if((uint)msgp + MSG_CTLLEN + 4 <= myproc()->sz)
      *(int*)((char*)(uintp)(uint)msgp + MSG_CTLLEN) = CMSG_HDRLEN + installed*4;
    if((uint)msgp + MSG_FLAGS + 4 <= myproc()->sz)
      *(int*)((char*)(uintp)(uint)msgp + MSG_FLAGS) = 0;
  } else {
    for(i = 0; i < nfds; i++)
      fileclose(fds[i]);
    if(ctlp != 0 && (uint)msgp + MSG_CTLLEN + 4 <= myproc()->sz)
      *(int*)((char*)(uintp)(uint)msgp + MSG_CTLLEN) = 0;
    if(nfds > 0 && (uint)msgp + MSG_FLAGS + 4 <= myproc()->sz)
      *(int*)((char*)(uintp)(uint)msgp + MSG_FLAGS) = MSG_CTRUNC_;
  }

  return off;
}
