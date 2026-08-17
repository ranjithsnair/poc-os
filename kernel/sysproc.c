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
#include "termios.h"
#include "vbe.h"
#include "fb.h"
#include "shm.h"

extern struct vbeinfo vbe;

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

// (fd, request, argp): TCGETS/TCSETS(W/F)/TIOCGWINSZ on the console
// device, or FBIOGET_VSCREENINFO (include/fb.h) on the framebuffer
// device - anything else, or any other device major, fails. fd->struct
// file lookup duplicates sysfile.c's static argfd() (not reachable
// from this file) rather than relocating the syscall there.
int
sys_ioctl(void)
{
  int fd, req;
  char *p;
  struct file *f;

  if(argint(0, &fd) < 0 || argint(1, &req) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;

  if(f->ip->major == FRAMEBUFFER){
    struct fb_info fi;

    if(req != FBIOGET_VSCREENINFO || vbe.magic != VBE_INFO_MAGIC)
      return -1;
    if(argptr(2, &p, sizeof(fi)) < 0)
      return -1;
    fi.xres = vbe.xres;
    fi.yres = vbe.yres;
    fi.pitch = vbe.pitch;
    fi.bpp = vbe.bpp;
    fi.red_mask_size = vbe.red_mask_size;
    fi.red_field_pos = vbe.red_field_pos;
    fi.green_mask_size = vbe.green_mask_size;
    fi.green_field_pos = vbe.green_field_pos;
    fi.blue_mask_size = vbe.blue_mask_size;
    fi.blue_field_pos = vbe.blue_field_pos;
    memmove(p, &fi, sizeof(fi));
    return 0;
  }

  if(f->ip->major != CONSOLE)
    return -1;

  switch(req){
  case TCGETS:
    if(argptr(2, &p, sizeof(struct termios)) < 0)
      return -1;
    consolegettermios((struct termios*)p);
    return 0;
  case TCSETS:
  case TCSETSW:
  case TCSETSF:
    if(argptr(2, &p, sizeof(struct termios)) < 0)
      return -1;
    consolesettermios((struct termios*)p);
    return 0;
  case TIOCGWINSZ:
    if(argptr(2, &p, sizeof(struct winsize)) < 0)
      return -1;
    consolegetwinsize((struct winsize*)p);
    return 0;
  case TIOCSWINSZ:
    // No real resize support (see consolegetwinsize()'s own comment) -
    // accept and ignore, the same way a real tty would accept it even
    // if nothing were listening for the corresponding SIGWINCH.
    return 0;
  default:
    return -1;
  }
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
       (f->type != FD_INODE && f->type != FD_SHM))
      return -1;
  }

  n = PGROUNDUP((uint)len);

  // Framebuffer device: map the real physical VRAM directly - not a
  // kalloc()'d anonymous page (the usual allocuvm() path below) and
  // not a copy of a file's own *content* (loaduvm()'s usual job) - the
  // whole point is that writes land on the same physical memory the
  // display hardware itself scans out. Must bypass allocuvm()/
  // loaduvm() entirely, not just branch after them: allocuvm() would
  // kalloc() and mappages() real anonymous pages into this same VA
  // range first, and mappages() panics on remapping an already-present
  // PTE.
  if(f && f->ip->major == FRAMEBUFFER){
    uint fbsize;

    if(vbe.magic != VBE_INFO_MAGIC || (flags & MAP_FIXED))
      return -1;
    fbsize = PGROUNDUP(vbe.pitch * vbe.yres);
    if(offset < 0 || (uint)offset + n > fbsize || (uint)offset + n < (uint)offset)
      return -1;
    base = curproc->sz;
    if(mapuvm_phys(curproc->pgdir, base, n,
                    PGROUNDDOWN(vbe.phys_base) + (uint)offset, PTE_W|PTE_U) < 0)
      return -1;
    curproc->sz = base + n;
    switchuvm(curproc);
    uvmsetperm(curproc->pgdir, base, n, (prot & PROT_WRITE) != 0);
    return base;
  }

  // Shared memory (kernel/shm.c, GUI roadmap phase 3): map the
  // object's own kalloc()'d pages directly, the same mapuvm_phys()
  // path the framebuffer branch above uses - two processes mmap()ing
  // the same underlying shmobj (same fd, or one received via
  // SCM_RIGHTS/dup()/fork()) land PTEs pointing at the same physical
  // pages. PTE_SHM (include/mmu.h) marks them so kernel/vm.c's
  // deallocuvm() knows not to kfree() them on unmap/exit - the object
  // is only actually freed once its own struct file's ref count hits
  // zero (kernel/shm.c's shmclose(), called from fileclose()).
  if(f && f->type == FD_SHM){
    uint shmsize, pgoff, i;

    if(flags & MAP_FIXED)
      return -1;
    shmsize = PGROUNDUP(f->shm->size);
    if(offset < 0 || (offset % PGSIZE) != 0 ||
       (uint)offset + n > shmsize || (uint)offset + n < (uint)offset)
      return -1;
    pgoff = (uint)offset / PGSIZE;
    base = curproc->sz;
    for(i = 0; i < n / PGSIZE; i++){
      if(mapuvm_phys(curproc->pgdir, base + i*PGSIZE, PGSIZE,
                      V2P(f->shm->pages[pgoff + i]), PTE_W|PTE_U|PTE_SHM) < 0)
        return -1;
    }
    curproc->sz = base + n;
    switchuvm(curproc);
    uvmsetperm(curproc->pgdir, base, n, (prot & PROT_WRITE) != 0);
    return base;
  }

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
    uint loadlen;

    ilock(f->ip);
    // Clamp to what the file actually has: a real mmap() lets a
    // caller map past a file's EOF (the tail just reads as zero,
    // never written back) - musl/ldso/dynlink.c's map_library()
    // relies on exactly this ("we map too much, possibly even more
    // than the length of the file... we will not use the invalid
    // part") for its initial whole-span reservation mmap, sized from
    // the ELF's addr_max/addr_min, not the file's own byte length.
    // loaduvm() below would otherwise readi() past ip->size, get a
    // short read, and fail the whole mmap() - the first real second
    // shared object (build/libgui.so, GUI roadmap phase 7) to load
    // via this path (unlike libc.so, loaded directly by exec.c's own
    // PT_INTERP code, never through here) is what exposed this: it
    // surfaced to userspace as a misleading EPERM, since musl's
    // __syscall_ret() maps any bare kernel -1 return to errno 1.
    // allocuvm() above already memset() zeroed every newly allocated
    // page, so simply not reading into the overhang leaves it zero,
    // exactly like a real mmap's beyond-EOF behavior.
    loadlen = (uint)offset >= f->ip->size ? 0 : f->ip->size - (uint)offset;
    if(loadlen > (uint)len)
      loadlen = (uint)len;
    if(loadlen > 0 && loaduvm(curproc->pgdir, (char*)(uintp)base, f->ip, (uint)offset, loadlen) < 0){
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

// Multi-user identity syscalls (include/syscall.h, include/proc.h's
// uid/gid/euid/egid/suid/sgid/umask fields). getters are trivial reads;
// setters follow real POSIX setuid()/setgid() semantics: a privileged
// caller (euid==0) may set real+effective+saved together to any value,
// an unprivileged caller may only move its effective id to its current
// real or saved id (the standard "temporarily drop, later regain"
// pattern) - never to an arbitrary uid. seteuid()/setegid() are the
// effective-only variant of the same rule.

int
sys_getuid(void)
{
  return myproc()->uid;
}

int
sys_geteuid(void)
{
  return myproc()->euid;
}

int
sys_getgid(void)
{
  return myproc()->gid;
}

int
sys_getegid(void)
{
  return myproc()->egid;
}

int
sys_setuid(void)
{
  int uid;
  struct proc *p = myproc();

  if(argint(0, &uid) < 0)
    return -1;
  if(p->euid == 0){
    p->uid = p->euid = p->suid = uid;
    return 0;
  }
  if(uid == p->uid || uid == p->suid){
    p->euid = uid;
    return 0;
  }
  return -1;
}

int
sys_seteuid(void)
{
  int uid;
  struct proc *p = myproc();

  if(argint(0, &uid) < 0)
    return -1;
  if(p->euid != 0 && uid != (int)p->uid && uid != (int)p->suid)
    return -1;
  p->euid = uid;
  return 0;
}

int
sys_setgid(void)
{
  int gid;
  struct proc *p = myproc();

  if(argint(0, &gid) < 0)
    return -1;
  if(p->euid == 0){
    p->gid = p->egid = p->sgid = gid;
    return 0;
  }
  if(gid == p->gid || gid == p->sgid){
    p->egid = gid;
    return 0;
  }
  return -1;
}

int
sys_setegid(void)
{
  int gid;
  struct proc *p = myproc();

  if(argint(0, &gid) < 0)
    return -1;
  if(p->euid != 0 && gid != (int)p->gid && gid != (int)p->sgid)
    return -1;
  p->egid = gid;
  return 0;
}

// Returns the previous mask, matching real umask(2).
int
sys_umask(void)
{
  int mask;
  struct proc *p = myproc();
  int old = p->umask;

  if(argint(0, &mask) < 0)
    return -1;
  p->umask = mask & 0777;
  return old;
}

// (struct rtcdate *r): see include/syscall.h - just forwards to
// kernel/lapic.c's cmostime(), which already did the real CMOS/RTC
// hardware reading, unused by anything until now.
int
sys_date(void)
{
  char *p;

  if(argptr(0, &p, sizeof(struct rtcdate)) < 0)
    return -1;
  cmostime((struct rtcdate*)p);
  return 0;
}

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
