// Process-related system calls: each sys_* here is what syscall.c's
// dispatch table (in syscall.c) calls for the matching SYS_xxx number,
// after which it's mostly a thin wrapper - argument fetching via
// argint()/argptr() (syscall.c), then the real work in proc.c.

#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "proc.h"
#include "syscall.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

#ifdef X64
// See include/syscall.h: poc-os has no per-thread ID distinct from
// pid, so this just reports the caller's pid and otherwise ignores its
// argument (a real Linux set_tid_address stores it for use by
// pthread_exit's futex wake, which doesn't apply here either).
int
sys_set_tid_address(void)
{
  return myproc()->pid;
}

// Stub: always "succeeds" without actually blocking or waking anyone.
// See include/syscall.h - real blocking-wait/wake semantics need
// poc-os to have threads to block one of first.
int
sys_futex(void)
{
  return 0;
}

// (addr): see include/syscall.h - real Linux brk() semantics (an
// absolute target, always returning the resulting break) rather than
// poc-os's own SYS_sbrk (a relative delta, returning the *old* break).
// Built directly on allocuvm/deallocuvm rather than growproc(), since
// growproc()'s "int n" parameter is a delta and brk's is not.
int
sys_brk(void)
{
  int addr;
  struct proc *curproc = myproc();
  uint newsz;

  if(argint(0, &addr) < 0)
    return -1;
  newsz = (uint)addr;
  if(newsz == 0)
    return curproc->sz;
  if(newsz > curproc->sz){
    if(allocuvm(curproc->pgdir, curproc->sz, newsz) == 0)
      return curproc->sz;  // failed: return the unchanged break
  } else if(newsz < curproc->sz){
    deallocuvm(curproc->pgdir, curproc->sz, newsz);
  }
  curproc->sz = newsz;
  switchuvm(curproc);
  return curproc->sz;
}

// Stub: always fails - see include/syscall.h.
int
sys_ioctl(void)
{
  return -1;
}

// Stub: always succeeds without doing anything - see include/syscall.h.
int
sys_madvise(void)
{
  return 0;
}

// Stub: always fails - see include/syscall.h.
int
sys_mremap(void)
{
  return -1;
}

// PROT_WRITE/MAP_FIXED: the same numeric values musl's <sys/mman.h>
// (and Linux) use for them - the only two bits sys_mmap()/
// sys_mprotect() below ever inspect (see their doc comments and
// SYS_mmap/SYS_mprotect in include/syscall.h).
#define PROT_WRITE 2
#define MAP_FIXED  0x10

// (addr, len, prot, flags, fd, offset): see include/syscall.h. Every
// mapping still lives inside the single contiguous [0, curproc->sz)
// region a poc-os process's address space has always been - there's
// no separate VMA list. addr == 0, or any addr >= the current sz,
// grows sz by PGROUNDUP(len) (the same growproc()/allocuvm() mechanism
// sbrk() and the old anonymous-only mmap used, which zero-fills the
// new pages on its own) and places the mapping at that new top.
// addr < sz is the MAP_FIXED overlay path a real dynamic linker needs
// (see musl/ldso/dynlink.c's map_library(): reserve a whole library's
// span with one addr==0 mmap, then MAP_FIXED sub-mmap each PT_LOAD
// segment into it) - addr/addr+len must land entirely inside that
// already-mapped range, and this just overlays new content onto it
// (read from fd at offset, or zeroed if fd == -1) instead of growing
// sz further.
int
sys_mmap(void)
{
  int len, prot, flags, fd, offset;
  int addr;
  uint base, n, sz;
  struct proc *curproc = myproc();
  struct file *f = 0;

  if(argint(0, &addr) < 0 || argint(1, &len) < 0 || argint(2, &prot) < 0 ||
     argint(3, &flags) < 0 || argint(4, &fd) < 0 || argint(5, &offset) < 0)
    return -1;
  if(len <= 0)
    return -1;
  if(fd != -1){
    if(fd < 0 || fd >= NOFILE || (f = curproc->ofile[fd]) == 0 ||
       f->type != FD_INODE)
      return -1;
  }

  n = PGROUNDUP((uint)len);
  if((flags & MAP_FIXED) && (uint)addr < curproc->sz){
    base = (uint)addr;
    if(base % PGSIZE != 0 || base + n > curproc->sz || base + n < base)
      return -1;
  } else {
    base = curproc->sz;
    if((sz = allocuvm(curproc->pgdir, curproc->sz, base + n)) == 0)
      return -1;
    curproc->sz = sz;
    switchuvm(curproc);
  }

  if(f){
    ilock(f->ip);
    if(loaduvm(curproc->pgdir, (char*)(uintp)base, f->ip, (uint)offset, (uint)len) < 0){
      iunlock(f->ip);
      return -1;
    }
    iunlock(f->ip);
  } else {
    uvmzero(curproc->pgdir, base, (uint)len);
  }
  uvmsetperm(curproc->pgdir, base, n, (prot & PROT_WRITE) != 0);
  return base;
}

// (addr, len): only succeeds when [addr, addr+PGROUNDUP(len)) is
// exactly the current top of the address space - see include/syscall.h.
int
sys_munmap(void)
{
  int addr, len;
  uint sz;
  struct proc *curproc = myproc();

  if(argint(0, &addr) < 0 || argint(1, &len) < 0)
    return -1;
  if(len <= 0)
    return -1;

  sz = PGROUNDUP((uint)len);
  if((uint)addr + sz != curproc->sz)
    return -1;
  if(growproc(-(int)sz) < 0)
    return -1;
  return 0;
}

// (addr, len, prot): see include/syscall.h and sys_mmap()'s doc
// comment above - addr/len are rounded out to whole pages the same
// way a real mprotect() would (POSIX doesn't require addr to already
// be page-aligned, only that it lie on one).
int
sys_mprotect(void)
{
  int addr, len, prot;
  uint base, n;
  struct proc *curproc = myproc();

  if(argint(0, &addr) < 0 || argint(1, &len) < 0 || argint(2, &prot) < 0)
    return -1;
  if(len <= 0)
    return -1;

  base = PGROUNDDOWN((uint)addr);
  n = PGROUNDUP((uint)addr + len) - base;
  if(base + n > curproc->sz || base + n < base)
    return -1;
  uvmsetperm(curproc->pgdir, base, n, (prot & PROT_WRITE) != 0);
  return 0;
}

// (code, addr): only ARCH_SET_FS is implemented (see include/syscall.h).
// Just records addr in curproc->tls_base - kernel/trap.c is what
// actually loads it into the FS_BASE MSR, on every return to user
// mode, since that MSR isn't part of the context swtch() saves/
// restores on a context switch.
int
sys_arch_prctl(void)
{
  int code;
  int addr;

  if(argint(0, &code) < 0 || argint(1, &addr) < 0)
    return -1;
  if(code != ARCH_SET_FS)
    return -1;
  myproc()->tls_base = (uintp)(uint)addr;
  return 0;
}
#endif

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
