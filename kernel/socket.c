// AF_UNIX stream socket mechanism (GUI roadmap phase 3) - see
// include/socket.h for the on-disk-free, message-boundary-preserving
// design rationale and the sockconn/socket struct layouts. Mirrors
// kernel/pipe.c's own shape closely: a shared, kalloc()'d, refcounted-
// by-open-flags object (struct sockconn here, struct pipe there) plus
// sleep()/wakeup() for blocking, and kernel/file.c's ftable[] pattern
// for struct socket's own fixed-size backing store (too small to
// deserve a whole kalloc()'d page each).
//
// kernel/sysnet.c holds the syscall-argument-parsing glue that calls
// into this file, exactly the way kernel/sysfile.c's sys_pipe() is
// thin glue around pipealloc() here.

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

static struct {
  struct spinlock lock;
  struct socket sock[NSOCKET];
} stable;

// path -> listening struct socket*, populated by sockbind(), consumed
// by sockconnect(). Not a real directory entry (see include/socket.h) -
// just a small in-kernel namespace, same shape as ftable/stable
// themselves.
static struct {
  struct spinlock lock;
  struct { char path[SUNPATHMAX]; struct socket *sock; } b[SOCKBINDMAX];
} sbind;

void
sockinit(void)
{
  initlock(&stable.lock, "stable");
  initlock(&sbind.lock, "sbind");
}

static struct socket*
sockalloc(void)
{
  struct socket *s;

  acquire(&stable.lock);
  for(s = stable.sock; s < stable.sock + NSOCKET; s++){
    if(!s->inuse){
      memset(s, 0, sizeof(*s));
      s->inuse = 1;
      release(&stable.lock);
      return s;
    }
  }
  release(&stable.lock);
  return 0;
}

// socket(2): a fresh, unbound/unconnected endpoint wrapped in a struct
// file - kernel/sysnet.c's sys_socket() is all argument checking
// (AF_UNIX/SOCK_STREAM only), the object itself doesn't care.
struct file*
sockcreate(void)
{
  struct socket *s;
  struct file *f;

  if((s = sockalloc()) == 0)
    return 0;
  if((f = filealloc()) == 0){
    s->inuse = 0;
    return 0;
  }
  f->type = FD_SOCK;
  f->readable = 1;
  f->writable = 1;
  f->sock = s;
  s->state = SOCK_UNBOUND;
  return f;
}

int
sockbind(struct socket *s, char *path)
{
  int i, free;

  if(s->state != SOCK_UNBOUND)
    return -1;

  acquire(&sbind.lock);
  free = -1;
  for(i = 0; i < SOCKBINDMAX; i++){
    if(sbind.b[i].sock == 0){
      if(free < 0)
        free = i;
      continue;
    }
    if(strncmp(sbind.b[i].path, path, SUNPATHMAX) == 0){
      release(&sbind.lock);
      return -1;  // address already in use
    }
  }
  if(free < 0){
    release(&sbind.lock);
    return -1;
  }
  safestrcpy(sbind.b[free].path, path, SUNPATHMAX);
  sbind.b[free].sock = s;
  release(&sbind.lock);

  safestrcpy(s->path, path, SUNPATHMAX);
  return 0;
}

int
socklisten(struct socket *s)
{
  if(s->state != SOCK_UNBOUND || s->path[0] == 0)
    return -1;
  initlock(&s->listenlock, "socklisten");
  s->state = SOCK_LISTENING;
  return 0;
}

int
sockconnect(struct socket *s, char *path)
{
  struct socket *ls;
  struct sockconn *c;
  int i;

  if(s->state != SOCK_UNBOUND)
    return -1;

  acquire(&sbind.lock);
  ls = 0;
  for(i = 0; i < SOCKBINDMAX; i++){
    if(sbind.b[i].sock && strncmp(sbind.b[i].path, path, SUNPATHMAX) == 0){
      ls = sbind.b[i].sock;
      break;
    }
  }
  release(&sbind.lock);
  if(ls == 0 || ls->state != SOCK_LISTENING)
    return -1;

  if((c = (struct sockconn*)kalloc()) == 0)
    return -1;
  memset(c, 0, sizeof(*c));
  initlock(&c->lock, "sockconn");
  c->open[0] = 1;  // this end (the connector)
  c->open[1] = 1;  // the future accept()ed end

  acquire(&ls->listenlock);
  if(ls->bcount == SOCKBACKLOG){
    release(&ls->listenlock);
    kfree((char*)c);
    return -1;  // backlog full - fail fast rather than block, see plan
  }
  ls->backlog[(ls->bhead + ls->bcount) % SOCKBACKLOG] = c;
  ls->bcount++;
  wakeup(ls);
  release(&ls->listenlock);

  s->state = SOCK_CONNECTED;
  s->conn = c;
  s->end = 0;
  return 0;
}

struct socket*
sockaccept(struct socket *ls)
{
  struct sockconn *c;
  struct socket *s;

  if(ls->state != SOCK_LISTENING)
    return 0;

  acquire(&ls->listenlock);
  while(ls->bcount == 0){
    if(myproc()->killed){
      release(&ls->listenlock);
      return 0;
    }
    sleep(ls, &ls->listenlock);
  }
  c = ls->backlog[ls->bhead];
  ls->bhead = (ls->bhead + 1) % SOCKBACKLOG;
  ls->bcount--;
  release(&ls->listenlock);

  if((s = sockalloc()) == 0){
    int stillopen;
    acquire(&c->lock);
    c->open[1] = 0;
    wakeup(c);
    stillopen = c->open[0];
    release(&c->lock);
    if(!stillopen)
      kfree((char*)c);
    return 0;
  }
  s->state = SOCK_CONNECTED;
  s->conn = c;
  s->end = 1;
  return s;
}

int
sockpair(struct socket **s0, struct socket **s1)
{
  struct sockconn *c;

  if((c = (struct sockconn*)kalloc()) == 0)
    return -1;
  memset(c, 0, sizeof(*c));
  initlock(&c->lock, "sockconn");
  c->open[0] = 1;
  c->open[1] = 1;

  if((*s0 = sockalloc()) == 0){
    kfree((char*)c);
    return -1;
  }
  if((*s1 = sockalloc()) == 0){
    (*s0)->inuse = 0;
    kfree((char*)c);
    return -1;
  }
  (*s0)->state = SOCK_CONNECTED;
  (*s0)->conn = c;
  (*s0)->end = 0;
  (*s1)->state = SOCK_CONNECTED;
  (*s1)->conn = c;
  (*s1)->end = 1;
  return 0;
}

// Queues one whole message (data + any passed fds, already filedup()'d
// by the caller - see kernel/sysnet.c's sys_sendmsg()) for the peer to
// pick up with sockrecv(). Blocks while the queue is full, exactly the
// way pipewrite() blocks on a full ring (kernel/pipe.c).
int
socksend(struct socket *s, char *data, int len, struct file **fds, int nfds)
{
  struct sockconn *c;
  struct sockmsg *m;
  int i, other;

  if(s->state != SOCK_CONNECTED || len < 0 || len > SOCKMSGSIZE || nfds > MAXFDMSG)
    return -1;
  c = s->conn;
  other = 1 - s->end;

  acquire(&c->lock);
  while(c->count[s->end] == SOCKQLEN){
    if(!c->open[other] || myproc()->killed){
      release(&c->lock);
      return -1;
    }
    wakeup(c);
    sleep(c, &c->lock);
  }
  if(!c->open[other]){
    release(&c->lock);
    return -1;  // peer gone
  }
  m = &c->q[s->end][(c->head[s->end] + c->count[s->end]) % SOCKQLEN];
  memmove(m->data, data, len);
  m->len = len;
  m->nfds = nfds;
  for(i = 0; i < nfds; i++)
    m->fds[i] = fds[i];
  c->count[s->end]++;
  wakeup(c);
  release(&c->lock);
  return len;
}

// Dequeues one whole message. *nfds is filled with however many fds
// actually rode along (capped at maxfds - any beyond that are closed
// here rather than leaked, matching real recvmsg()'s MSG_CTRUNC
// behavior of dropping what doesn't fit).
int
sockrecv(struct socket *s, char *data, int maxlen, struct file **fds, int *nfds, int maxfds)
{
  struct sockconn *c;
  struct sockmsg *m;
  int i, other, n, nf;

  if(s->state != SOCK_CONNECTED)
    return -1;
  c = s->conn;
  other = 1 - s->end;

  acquire(&c->lock);
  while(c->count[other] == 0){
    if(!c->open[other]){
      release(&c->lock);
      *nfds = 0;
      return 0;  // peer closed: EOF
    }
    if(myproc()->killed){
      release(&c->lock);
      return -1;
    }
    sleep(c, &c->lock);
  }
  m = &c->q[other][c->head[other]];
  n = m->len < maxlen ? m->len : maxlen;
  memmove(data, m->data, n);
  nf = m->nfds < maxfds ? m->nfds : maxfds;
  for(i = 0; i < nf; i++)
    fds[i] = m->fds[i];
  for(i = nf; i < m->nfds; i++)
    fileclose(m->fds[i]);
  *nfds = nf;
  c->head[other] = (c->head[other] + 1) % SOCKQLEN;
  c->count[other]--;
  wakeup(c);
  release(&c->lock);
  return n;
}

// Readiness checks for kernel/epoll.c's fdready(). A listening socket
// is "readable" when it has a pending connection to accept(); a
// connected endpoint is readable when its peer has queued a message or
// has closed (EOF - matches real epoll, where a closed peer makes the
// fd immediately ready rather than blocking forever).
int
sockreadable(struct socket *s)
{
  struct sockconn *c;
  int other, r;

  if(s->state == SOCK_LISTENING){
    acquire(&s->listenlock);
    r = s->bcount > 0;
    release(&s->listenlock);
    return r;
  }
  if(s->state != SOCK_CONNECTED)
    return 0;
  c = s->conn;
  other = 1 - s->end;
  acquire(&c->lock);
  r = (c->count[other] > 0) || !c->open[other];
  release(&c->lock);
  return r;
}

int
sockwritable(struct socket *s)
{
  struct sockconn *c;
  int other, r;

  if(s->state != SOCK_CONNECTED)
    return 0;
  c = s->conn;
  other = 1 - s->end;
  acquire(&c->lock);
  r = (c->count[s->end] < SOCKQLEN) || !c->open[other];
  release(&c->lock);
  return r;
}

// Called from kernel/file.c's fileclose() once a FD_SOCK file's last
// reference goes away.
void
sockclose(struct socket *s)
{
  struct file *toclose[2 * SOCKQLEN * MAXFDMSG];
  int nclose = 0;
  int i, j, bothclosed;

  if(s->state == SOCK_CONNECTED && s->conn){
    struct sockconn *c = s->conn;

    acquire(&c->lock);
    c->open[s->end] = 0;
    wakeup(c);
    bothclosed = !c->open[0] && !c->open[1];
    if(bothclosed){
      // Drain any messages neither end ever read, closing their
      // passed fds rather than leaking the reference filedup() took.
      for(i = 0; i < 2; i++){
        while(c->count[i] > 0){
          struct sockmsg *m = &c->q[i][c->head[i]];
          for(j = 0; j < m->nfds; j++)
            toclose[nclose++] = m->fds[j];
          c->head[i] = (c->head[i] + 1) % SOCKQLEN;
          c->count[i]--;
        }
      }
    }
    release(&c->lock);
    for(i = 0; i < nclose; i++)
      fileclose(toclose[i]);
    if(bothclosed)
      kfree((char*)c);
  } else if(s->state == SOCK_LISTENING){
    acquire(&s->listenlock);
    while(s->bcount > 0){
      struct sockconn *c = s->backlog[s->bhead];
      int clientopen;

      s->bhead = (s->bhead + 1) % SOCKBACKLOG;
      s->bcount--;
      acquire(&c->lock);
      c->open[1] = 0;
      wakeup(c);
      clientopen = c->open[0];
      release(&c->lock);
      if(!clientopen)
        kfree((char*)c);
    }
    release(&s->listenlock);

    acquire(&sbind.lock);
    for(i = 0; i < SOCKBINDMAX; i++){
      if(sbind.b[i].sock == s){
        sbind.b[i].sock = 0;
        break;
      }
    }
    release(&sbind.lock);
  }
  s->inuse = 0;
}
