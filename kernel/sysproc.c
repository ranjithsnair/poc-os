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

// (addr, len, prot, flags, fd, offset): addr/prot/flags are ignored
// (see include/syscall.h) and only anonymous mappings (fd == -1) are
// supported - every mapping just grows curproc->sz by
// PGROUNDUP(len), the same mechanism sbrk() uses (growproc(), which
// allocuvm()s the new pages), and is placed at whatever the current
// top of the address space happens to be. Good enough for musl's
// allocator and TLS block, which only ever want fresh anonymous
// memory; nowhere close to a real mmap (no VMA list, no fixed
// addresses, no file backing) - that's future work, not attempted
// here.
int
sys_mmap(void)
{
  int len, prot, flags, fd, offset;
  int addr;
  uint sz;

  if(argint(1, &len) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 ||
     argint(4, &fd) < 0 || argint(5, &offset) < 0)
    return -1;
  if(fd != -1 || len <= 0)
    return -1;

  sz = PGROUNDUP((uint)len);
  addr = myproc()->sz;
  if(growproc((int)sz) < 0)
    return -1;
  return addr;
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
