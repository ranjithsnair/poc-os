// exec(): replace the calling process's memory image with a new program
// loaded from an ELF binary on disk. Builds the new address space
// entirely in a fresh page table first, and only swaps the process over
// to it (the "Commit" section near the bottom) once every step that can
// fail has already succeeded - so a bad executable, a truncated file, or
// running out of memory partway through just returns -1 and leaves the
// caller's original memory image untouched, instead of leaving the
// process half-replaced.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"
#include "auxv.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc;
  uintp sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

#ifdef X64
  // Push argument strings, then the argv[] pointer array itself -
  // ustack[3..] becomes that array, once the strings' final addresses
  // are all known. Unlike the 32-bit build, there's no fake-return-PC/
  // argc/argv-pointer header to also push here: main() is entered
  // directly (via iretq, not a call, so nothing reads a return
  // address off the stack) with argc/argv passed the same way any
  // SysV AMD64 call would - in %rdi/%rsi, set directly in the
  // trapframe below - rather than read off the stack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~7;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // The SysV AMD64 ABI guarantees %rsp % 16 == 0 at a fresh process's
  // entry point; some compiler-generated code (SSE instructions in
  // particular) relies on that.
  sp -= (argc+1) * sizeof(uintp);
  sp &= ~0xF;
  if(copyout(pgdir, sp, ustack, (argc+1)*sizeof(uintp)) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;
  curproc->tf->rdi = argc;
  curproc->tf->rsi = sp;         // argv
  // A fresh address space invalidates any old %fs base - it pointed
  // into memory that's about to be freed below.
  curproc->tls_base = 0;
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;
#else
  // Push argument strings, prepare rest of stack in ustack.
  // Copies each argv[] string itself onto the new stack (rounding sp
  // down to a 4-byte boundary each time) and records where it landed;
  // ustack[3..] then becomes the argv[] pointer array that follows,
  // once the strings' final addresses are all known.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  // Lay out the rest of the initial stack frame exactly as main()'s
  // caller (crt-style startup code in initcode.asm/usys.asm) expects to
  // find it: a fake return PC (main is never supposed to return, so this
  // is never actually used), then argc, then a pointer to the argv[]
  // array that ustack[3..] holds.
  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;
#endif

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

#ifdef X64
// Like exec(), but for musl-linked binaries rather than poc-os's own
// native ones: takes a third (envp) argument, and instead of handing
// argc/argv to the new program in %rdi/%rsi, builds the Linux-shaped
// stack image musl's crt1 (_start in musl/arch/x86_64/crt_arch.h)
// expects to find at the initial %rsp -
//   [argc][argv[0..argc-1]][NULL][envp[0..envc-1]][NULL][auxv pairs][AT_NULL,0]
// followed by the string/aux data those pointers reference, all above
// %rsp in memory (the pointer block is what's closest to the top of
// the stack, i.e. lowest address, since the stack grows down from sz).
//
// A separate function from exec() - sharing its ELF-loading loop would
// have meant threading a stack-layout choice through the single 64-bit
// exec() body, and this file already carries a from-scratch 32-bit
// exec() variant behind #ifdef X64/#else, so one more full variant here
// follows existing precedent rather than fighting it.
int
execve(char *path, char **argv, char **envp)
{
  char *s, *last;
  int i, n, off;
  uint argc, envc;
  uintp sz, sp;
  uintp argv_addr[MAXARG], envp_addr[MAXENVP];
  uintp random_addr, execfn_addr;
  uintp ustack[1 + (MAXARG+1) + (MAXENVP+1) + 2*16];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();
  // Not a real source of entropy - poc-os has no RNG yet - just a
  // stable 16-byte block for AT_RANDOM to point at. musl only
  // dereferences it from __init_ssp(), which is a no-op in this build
  // (stack-protector is disabled kernel- and userspace-wide).
  static uchar randbuf[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("execve: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  sp -= sizeof(randbuf);
  sp &= ~7;
  if(copyout(pgdir, sp, randbuf, sizeof(randbuf)) < 0)
    goto bad;
  random_addr = sp;

  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~7;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    argv_addr[argc] = sp;
  }
  execfn_addr = argc > 0 ? argv_addr[0] : 0;

  for(envc = 0; envp && envp[envc]; envc++) {
    if(envc >= MAXENVP)
      goto bad;
    sp = (sp - (strlen(envp[envc]) + 1)) & ~7;
    if(copyout(pgdir, sp, envp[envc], strlen(envp[envc]) + 1) < 0)
      goto bad;
    envp_addr[envc] = sp;
  }

  n = 0;
  ustack[n++] = argc;
  for(i = 0; i < (int)argc; i++)
    ustack[n++] = argv_addr[i];
  ustack[n++] = 0;
  for(i = 0; i < (int)envc; i++)
    ustack[n++] = envp_addr[i];
  ustack[n++] = 0;
#define AUXENT(t, v) do { ustack[n++] = (t); ustack[n++] = (uintp)(v); } while(0)
  // AT_PHDR assumes the program headers ended up loaded at the same
  // address as their file offset - true for a normal non-PIE static
  // link (base address 0), which is all poc-os's toolchain produces
  // today; a real load-bias calculation only matters once PIE/ET_DYN
  // binaries (dynamic linking) are in the picture.
  AUXENT(AT_PHDR, elf.phoff);
  AUXENT(AT_PHENT, elf.phentsize);
  AUXENT(AT_PHNUM, elf.phnum);
  AUXENT(AT_PAGESZ, PGSIZE);
  AUXENT(AT_BASE, 0);
  AUXENT(AT_ENTRY, elf.entry);
  AUXENT(AT_UID, 0);
  AUXENT(AT_EUID, 0);
  AUXENT(AT_GID, 0);
  AUXENT(AT_EGID, 0);
  AUXENT(AT_HWCAP, 0);
  AUXENT(AT_SECURE, 0);
  AUXENT(AT_RANDOM, random_addr);
  AUXENT(AT_EXECFN, execfn_addr);
  AUXENT(AT_NULL, 0);
#undef AUXENT

  sp -= n * sizeof(uintp);
  sp &= ~0xF;
  if(copyout(pgdir, sp, ustack, n * sizeof(uintp)) < 0)
    goto bad;

  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;
  curproc->tf->esp = sp;
  // A fresh address space invalidates any old %fs base - it pointed
  // into memory that's about to be freed below.
  curproc->tls_base = 0;
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}
#endif
