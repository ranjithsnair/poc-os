// POSIX-ish shared memory (GUI roadmap phase 3) - see include/shm.h
// for why this is a dedicated syscall rather than real shm_open().
//
// The one real subtlety here (see the plan doc): shm pages are
// ordinary kalloc()'d RAM, unlike phase 2's framebuffer mapping (real
// VRAM, always >= PHYSTOP). If kernel/vm.c's deallocuvm() freed them
// on every unmap/exit the way it does normal anonymous memory, the
// first process to unmap or exit would free memory a second process
// still has mapped. Fixed by marking these PTEs PTE_SHM (include/
// mmu.h) - deallocuvm() skips kfree() on them, and the pages are freed
// only here, in shmclose(), once the object's own refcnt (one per
// struct file referencing it - fork()/dup()/SCM_RIGHTS all bump it via
// filedup(), exactly like every other refcounted file object in this
// kernel) reaches zero.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "shm.h"

static struct {
  struct spinlock lock;
  struct shmobj obj[SHM_MAXOBJS];
} shmtable;

void
shminit(void)
{
  initlock(&shmtable.lock, "shmtable");
}

// SYS_shm_create(size): allocate a new object, its own kalloc()'d
// pages, and a struct file wrapping it. Every page is allocated
// up front - this kernel has no demand-paging/fault handler, so
// nothing can be filled in lazily on first touch the way a real
// shm_open()+ftruncate()+mmap() sequence would.
struct file*
shmcreate(uint size)
{
  struct shmobj *o;
  struct file *f;
  int npages, i;

  if(size == 0 || size > SHM_MAXPAGES * PGSIZE)
    return 0;
  npages = PGROUNDUP(size) / PGSIZE;

  acquire(&shmtable.lock);
  for(o = shmtable.obj; o < shmtable.obj + SHM_MAXOBJS; o++){
    if(!o->inuse){
      o->inuse = 1;
      break;
    }
  }
  release(&shmtable.lock);
  if(o == shmtable.obj + SHM_MAXOBJS)
    return 0;

  o->size = size;
  for(i = 0; i < npages; i++){
    if((o->pages[i] = kalloc()) == 0){
      while(--i >= 0)
        kfree(o->pages[i]);
      o->inuse = 0;
      return 0;
    }
    memset(o->pages[i], 0, PGSIZE);
  }

  if((f = filealloc()) == 0){
    for(i = 0; i < npages; i++)
      kfree(o->pages[i]);
    o->inuse = 0;
    return 0;
  }
  f->type = FD_SHM;
  f->readable = 1;
  f->writable = 1;
  f->shm = o;
  return f;
}

// Called from kernel/file.c's fileclose() once a FD_SHM file's own
// ref count reaches zero - see include/shm.h's comment on struct
// shmobj for why that's already the right refcount to key off of,
// with no separate one needed here.
void
shmclose(struct shmobj *o)
{
  int i, npages;

  npages = PGROUNDUP(o->size) / PGSIZE;
  for(i = 0; i < npages; i++)
    kfree(o->pages[i]);
  o->inuse = 0;
}

// (size): see include/shm.h - not a real Linux/musl syscall, called
// directly via syscall(SYS_shm_create, size).
int
sys_shm_create(void)
{
  int size;
  struct file *f;
  int fd;

  if(argint(0, &size) < 0 || size <= 0)
    return -1;
  if((f = shmcreate((uint)size)) == 0)
    return -1;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -1;
  }
  return fd;
}
