// System call dispatch and the argument-fetching helpers every sys_*
// handler (in sysproc.c and sysfile.c) uses to safely read its
// arguments out of user memory.
//
// User code makes a system call with INT T_SYSCALL.
// System call number in %eax.
// Arguments on the stack, from the user call to the C
// library system call function. The saved user %esp points
// to a saved program counter, and then the first argument.
//
// Every fetch* / arg* function below treats curproc->tf->esp and any
// pointer argument as untrusted: it bounds-checks against curproc->sz
// before dereferencing, since a user program can pass any garbage value
// it likes and the kernel must never fault (or worse, be tricked into
// reading/writing outside its own address space) because of it.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "syscall.h"

// Fetch the int at addr from the current process.
int
fetchint(uintp addr, int *ip)
{
  struct proc *curproc = myproc();

  if(addr >= curproc->sz || addr+4 > curproc->sz)
    return -1;
  *ip = *(int*)(addr);
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Doesn't actually copy the string - just sets *pp to point at it.
// Returns length of string, not including nul.
int
fetchstr(uintp addr, char **pp)
{
  char *s, *ep;
  struct proc *curproc = myproc();

  if(addr >= curproc->sz)
    return -1;
  *pp = (char*)addr;
  ep = (char*)curproc->sz;
  for(s = *pp; s < ep; s++){
    if(*s == 0)
      return s - *pp;
  }
  return -1;
}

#ifdef X64
// Fetch the nth system call argument. User code makes a system call
// with INT T_SYSCALL, syscall number in %eax, and - since usys64.asm's
// stubs are ordinary C-callable functions - arguments already in
// %rdi/%rsi/%rdx/%rcx/%r8/%r9 per the SysV AMD64 calling convention
// (the same registers alltraps in trapasm64.asm saved into the
// trapframe), rather than on the user stack the way the 32-bit build's
// argint reads them. int (not %rcx) is safe to use here as arg 4
// because this is an INT-based syscall, not the `syscall` instruction -
// only the latter clobbers %rcx/%r11 on entry.
int
argint(int n, int *ip)
{
  struct trapframe *tf = myproc()->tf;
  uintp v;

  switch(n){
  case 0: v = tf->rdi; break;
  case 1: v = tf->rsi; break;
  case 2: v = tf->rdx; break;
  case 3: v = tf->rcx; break;
  case 4: v = tf->r8;  break;
  case 5: v = tf->r9;  break;
  default: return -1;
  }
  *ip = (int)v;
  return 0;
}
#else
// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  return fetchint((myproc()->tf->esp) + 4 + 4*n, ip);
}
#endif

// Fetch the nth word-sized system call argument as a pointer
// to a block of memory of size bytes.  Check that the pointer
// lies within the process address space.
int
argptr(int n, char **pp, int size)
{
  int i;
  struct proc *curproc = myproc();
 
  if(argint(n, &i) < 0)
    return -1;
  if(size < 0 || (uintp)i >= curproc->sz || (uintp)i+size > curproc->sz)
    return -1;
  *pp = (char*)(uintp)i;
  return 0;
}

// Fetch the nth word-sized system call argument as a string pointer.
// Check that the pointer is valid and the string is nul-terminated.
// (There is no shared writable memory, so the string can't change
// between this check and being used by the kernel.)
int
argstr(int n, char **pp)
{
  int addr;
  if(argint(n, &addr) < 0)
    return -1;
  return fetchstr(addr, pp);
}

extern int sys_chdir(void);
extern int sys_close(void);
extern int sys_dup(void);
#ifdef X64
extern int sys_execve(void);
extern int sys_arch_prctl(void);
extern int sys_mmap(void);
extern int sys_munmap(void);
extern int sys_set_tid_address(void);
extern int sys_futex(void);
extern int sys_brk(void);
extern int sys_madvise(void);
extern int sys_mremap(void);
extern int sys_writev(void);
extern int sys_ioctl(void);
extern int sys_lseek(void);
extern int sys_mprotect(void);
extern int sys_pread(void);
extern int sys_fcntl(void);
extern int sys_readv(void);
#endif
extern int sys_exec(void);
extern int sys_exit(void);
extern int sys_fork(void);
extern int sys_fstat(void);
extern int sys_getpid(void);
extern int sys_kill(void);
extern int sys_link(void);
extern int sys_mkdir(void);
extern int sys_mknod(void);
extern int sys_open(void);
extern int sys_pipe(void);
extern int sys_read(void);
extern int sys_sbrk(void);
extern int sys_sleep(void);
extern int sys_unlink(void);
extern int sys_wait(void);
extern int sys_write(void);
extern int sys_uptime(void);

static int (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
#ifdef X64
[SYS_execve]            sys_execve,
[SYS_arch_prctl]        sys_arch_prctl,
[SYS_mmap]              sys_mmap,
[SYS_munmap]            sys_munmap,
[SYS_exit_group]        sys_exit,  // see include/syscall.h
[SYS_set_tid_address]   sys_set_tid_address,
[SYS_futex]             sys_futex,
[SYS_brk]               sys_brk,
[SYS_madvise]           sys_madvise,
[SYS_mremap]            sys_mremap,
[SYS_writev]            sys_writev,
[SYS_ioctl]             sys_ioctl,
[SYS_lseek]             sys_lseek,
[SYS_mprotect]          sys_mprotect,
[SYS_pread]             sys_pread,
[SYS_fcntl]             sys_fcntl,
[SYS_readv]             sys_readv,
#endif
};

// Called from trap() for a T_SYSCALL trap. tf->eax holds the syscall
// number the user-space stub (usys.asm) placed there; the handler's
// return value replaces it, becoming the value the user-space call
// itself returns once we resume.
void
syscall(void)
{
  int num;
  struct proc *curproc = myproc();

  num = curproc->tf->eax;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    curproc->tf->eax = syscalls[num]();
  } else {
    cprintf("%d %s: unknown sys call %d\n",
            curproc->pid, curproc->name, num);
    curproc->tf->eax = -1;
  }
}
