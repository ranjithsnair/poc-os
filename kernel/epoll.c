// epoll (GUI roadmap phase 3) - see the plan doc's stated scope cut:
// this is a bounded poll loop, not real per-fd wait queues (this
// xv6-derived kernel has no multi-channel sleep to build true
// readiness notification on). epoll_pwait() reuses the exact same
// ticks/tickslock global tick counter and sleep channel kernel/
// sysproc.c's sys_sleep() already sleeps on, just rechecking every
// watched fd's readiness each time it wakes instead of spinning.
//
// struct epoll_event isn't available to kernel code (no musl headers
// here) - its layout is hand-derived from musl/include/sys/epoll.h:
// on x86_64 it's __attribute__((packed)), so `uint32_t events` sits at
// offset 0 and the 8-byte epoll_data_t union at offset 4 with no
// padding (matches real Linux's own historical x86_64 ABI quirk here).

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

#define EPOLL_MAXFDS 16   // matches NOFILE - can't watch more fds than
                           // a process can even have open

#define EPOLLIN  0x001
#define EPOLLOUT 0x004
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

struct epitem {
  int inuse;
  int fd;
  int events;
  int datalo, datahi;   // opaque epoll_data_t, stored as raw halves
};

struct epollfd {
  struct spinlock lock;
  struct epitem items[EPOLL_MAXFDS];
};

// Readiness for one fd, by file type - pipes and sockets have real
// queued/available-space state to check (kernel/pipe.c's
// pipereadable()/pipewritable(), kernel/socket.c's sockreadable()/
// sockwritable()); anything else (regular files, console, framebuffer,
// ...) is always considered ready, the common minimal-epoll shortcut
// (matches how this kernel already treats regular-file mmap/read as
// never truly blocking).
static int
fdready(struct file *f, int forwrite)
{
  if(f == 0)
    return 0;
  switch(f->type){
  case FD_PIPE:
    return forwrite ? pipewritable(f->pipe) : pipereadable(f->pipe);
  case FD_SOCK:
    return forwrite ? sockwritable(f->sock) : sockreadable(f->sock);
  default:
    return 1;
  }
}

int
sys_epoll_create1(void)
{
  int flags;
  struct epollfd *e;
  struct file *f;
  int fd;

  if(argint(0, &flags) < 0)
    return -1;
  if((e = (struct epollfd*)kalloc()) == 0)
    return -1;
  memset(e, 0, sizeof(*e));
  initlock(&e->lock, "epoll");
  if((f = filealloc()) == 0){
    kfree((char*)e);
    return -1;
  }
  f->type = FD_EPOLL;
  f->readable = 1;
  f->writable = 1;
  f->epoll = e;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -1;
  }
  return fd;
}

int
sys_epoll_ctl(void)
{
  struct file *ef;
  int op, fd, evp;
  struct epollfd *e;
  int i, events, lo, hi;

  if(argfd(0, 0, &ef) < 0 || argint(1, &op) < 0 || argint(2, &fd) < 0 || argint(3, &evp) < 0)
    return -1;
  if(ef->type != FD_EPOLL)
    return -1;
  e = ef->epoll;
  if(fd < 0 || fd >= NOFILE || myproc()->ofile[fd] == 0)
    return -1;

  if(op == EPOLL_CTL_DEL){
    acquire(&e->lock);
    for(i = 0; i < EPOLL_MAXFDS; i++)
      if(e->items[i].inuse && e->items[i].fd == fd)
        e->items[i].inuse = 0;
    release(&e->lock);
    return 0;
  }

  if(fetchint(evp, &events) < 0 || fetchint(evp + 4, &lo) < 0 || fetchint(evp + 8, &hi) < 0)
    return -1;

  acquire(&e->lock);
  if(op == EPOLL_CTL_MOD){
    for(i = 0; i < EPOLL_MAXFDS; i++){
      if(e->items[i].inuse && e->items[i].fd == fd){
        e->items[i].events = events;
        e->items[i].datalo = lo;
        e->items[i].datahi = hi;
        release(&e->lock);
        return 0;
      }
    }
    release(&e->lock);
    return -1;
  }
  if(op == EPOLL_CTL_ADD){
    int free = -1;
    for(i = 0; i < EPOLL_MAXFDS; i++){
      if(e->items[i].inuse && e->items[i].fd == fd){
        release(&e->lock);
        return -1;  // already registered
      }
      if(!e->items[i].inuse && free < 0)
        free = i;
    }
    if(free < 0){
      release(&e->lock);
      return -1;  // interest list full
    }
    e->items[free].inuse = 1;
    e->items[free].fd = fd;
    e->items[free].events = events;
    e->items[free].datalo = lo;
    e->items[free].datahi = hi;
    release(&e->lock);
    return 0;
  }
  release(&e->lock);
  return -1;
}

// (epfd, events, maxevents, timeout_ms, sigmask - ignored, see plan's
// scope cuts). timeout is converted to a bounded number of ticks;
// ticks aren't calibrated to a real time source in this kernel (see
// kernel/lapic.c's own uncalibrated TICR), so this is necessarily
// approximate, same limitation sys_sleep() already has.
int
sys_epoll_pwait(void)
{
  struct file *ef;
  int eventsp, maxevents, timeout;
  struct epollfd *e;
  uint deadline;
  int i, out;

  if(argfd(0, 0, &ef) < 0 || argint(1, &eventsp) < 0 || argint(2, &maxevents) < 0 ||
     argint(3, &timeout) < 0)
    return -1;
  if(ef->type != FD_EPOLL || maxevents <= 0)
    return -1;
  e = ef->epoll;

  acquire(&tickslock);
  deadline = ticks + (timeout < 0 ? 0 : (uint)(timeout + 9) / 10);
  release(&tickslock);

  for(;;){
    out = 0;
    acquire(&e->lock);
    for(i = 0; i < EPOLL_MAXFDS && out < maxevents; i++){
      struct file *tf;
      int rd, wr, rev;

      if(!e->items[i].inuse)
        continue;
      tf = myproc()->ofile[e->items[i].fd];
      if(tf == 0)
        continue;  // fd closed since ADD - just skip it
      rd = (e->items[i].events & EPOLLIN) && fdready(tf, 0);
      wr = (e->items[i].events & EPOLLOUT) && fdready(tf, 1);
      if(!rd && !wr)
        continue;
      if((uint)eventsp + 12*out + 12 > myproc()->sz)
        break;
      rev = (rd ? EPOLLIN : 0) | (wr ? EPOLLOUT : 0);
      *(int*)((char*)(uintp)(uint)eventsp + 12*out) = rev;
      *(int*)((char*)(uintp)(uint)eventsp + 12*out + 4) = e->items[i].datalo;
      *(int*)((char*)(uintp)(uint)eventsp + 12*out + 8) = e->items[i].datahi;
      out++;
    }
    release(&e->lock);

    if(out > 0)
      return out;

    acquire(&tickslock);
    if(timeout >= 0 && ticks >= deadline){
      release(&tickslock);
      return 0;
    }
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
    release(&tickslock);
  }
}
