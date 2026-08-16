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
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "elf.h"
#include "auxv.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc;
  uintp sz, sp, ustack[MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();
  int su_uid = -1, su_gid = -1;

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  if(permcheck(ip, curproc->euid, curproc->egid, PERM_X) < 0)
    goto bad;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // setuid/setgid-on-exec: honor S_ISUID(04000)/S_ISGID(02000) in the
  // binary's own mode bits, captured now while ip is still locked and
  // known-good, but only actually applied to curproc's identity at the
  // "Commit" section below, once every failure-capable step has already
  // succeeded - same reasoning as every other piece of new-process state
  // this function builds up before committing.
  if(ip->mode & 04000)
    su_uid = ip->uid;
  if(ip->mode & 02000)
    su_gid = ip->gid;

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

  // Allocate a guard page plus USTACKPAGES usable pages at the next
  // page boundary. Make the guard page (the lowest of the bunch)
  // inaccessible; the rest is the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + (USTACKPAGES+1)*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - (USTACKPAGES+1)*PGSIZE));
  sp = sz;

  // Push argument strings, then the argv[] pointer array itself -
  // ustack[3..] becomes that array, once the strings' final addresses
  // are all known. There's no fake-return-PC/argc/argv-pointer header
  // to also push here: main() is entered directly (via iretq, not a
  // call, so nothing reads a return address off the stack) with argc/
  // argv passed the same way any SysV AMD64 call would - in %rdi/%rsi,
  // set directly in the trapframe below - rather than read off the stack.
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
  if(su_uid >= 0)
    curproc->euid = curproc->suid = su_uid;
  if(su_gid >= 0)
    curproc->egid = curproc->sgid = su_gid;
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
// A separate function from exec() rather than threading a stack-layout
// choice through the single exec() body.
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
  int su_uid = -1, su_gid = -1;
  // PT_INTERP support: a PT_LOAD-only ELF (every poc-os binary until
  // now) still runs exactly as before, entered directly at elf.entry
  // with AT_BASE 0. A PT_INTERP segment instead names a dynamic
  // linker (e.g. "/ld-musl-x86_64.so.1") to load *alongside* the main
  // executable and jump to instead - its own struct elfhdr/proghdr/
  // inode, so the main executable's elf/ph/ip (needed afterwards for
  // AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY) are never overwritten. The
  // interpreter is ET_DYN (position-independent) - interp_bias is the
  // load address the kernel picks for it, placed right above the main
  // executable in the same contiguous [0, sz) region every poc-os
  // process address space already is (see sys_mmap's doc comment in
  // include/syscall.h for why that's also where the interpreter's own
  // runtime mmap calls, e.g. for libc.so, end up landing).
  struct elfhdr ielf;
  struct proghdr iph;
  struct inode *iip = 0;
  char interp_path[64];
  int has_interp = 0;
  uintp interp_bias = 0, interp_entry = 0;
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

  if(permcheck(ip, curproc->euid, curproc->egid, PERM_X) < 0)
    goto bad;

  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // setuid/setgid-on-exec: see exec()'s identical comment above - same
  // reasoning (captured now while ip is locked, applied only at Commit).
  // This is what lets su (installed mode 4755, owned by root - see
  // bash/poc/su.c and mkfs/mkfs.c's install_mode_override()) briefly
  // regain root's identity when run by a non-root caller.
  if(ip->mode & 04000)
    su_uid = ip->uid;
  if(ip->mode & 02000)
    su_gid = ip->gid;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type == ELF_PROG_INTERP){
      // ph.filesz includes the interpreter path's NUL terminator (the
      // linker always emits one) - reject anything that wouldn't leave
      // room for it rather than silently truncating a path.
      if(ph.filesz == 0 || ph.filesz > sizeof(interp_path))
        goto bad;
      if(readi(ip, interp_path, ph.off, ph.filesz) != ph.filesz)
        goto bad;
      if(interp_path[ph.filesz-1] != 0)
        goto bad;
      has_interp = 1;
      continue;
    }
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    // No "vaddr must be page-aligned" check - see the identical
    // comment on the interpreter-loading loop below, which applies
    // here too: a PIE (ET_DYN) main executable, not just a shared
    // library, routinely has a second-and-later PT_LOAD segment
    // that isn't page-aligned, and loaduvm() (kernel/vm.c) handles
    // that correctly. Every poc-os binary this loop has ever loaded
    // until now happened to be page-aligned already (a single,
    // merged, OMAGIC-style LOAD segment - see the Makefile's -N
    // linking), so this is a pure relaxation: nothing that passed
    // before behaves any differently now.
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Load the interpreter, if any, right above the main executable -
  // still within the same contiguous sz region, so copyuvm()/
  // deallocuvm()/freevm() (which all assume the whole live address
  // space is exactly [0, sz)) need no changes at all to also cover it.
  if(has_interp){
    begin_op();
    if((iip = namei(interp_path)) == 0){
      end_op();
      goto bad;
    }
    ilock(iip);
    if(readi(iip, (char*)&ielf, 0, sizeof(ielf)) != sizeof(ielf))
      goto bad;
    if(ielf.magic != ELF_MAGIC)
      goto bad;
    interp_bias = PGROUNDUP(sz);
    for(i=0, off=ielf.phoff; i<ielf.phnum; i++, off+=sizeof(iph)){
      if(readi(iip, (char*)&iph, off, sizeof(iph)) != sizeof(iph))
        goto bad;
      if(iph.type != ELF_PROG_LOAD)
        continue;
      if(iph.memsz < iph.filesz)
        goto bad;
      if(interp_bias + iph.vaddr + iph.memsz < interp_bias)
        goto bad;
      // Unlike the main executable's own PT_LOAD loop above, no
      // "vaddr must be page-aligned" check here - a real interpreter
      // (like a real shared library generally) routinely has a
      // second-and-later PT_LOAD segment that isn't, and loaduvm()
      // (kernel/vm.c) now handles that correctly. allocuvm() already
      // covers any leading partial page too: it always allocates
      // starting from PGROUNDUP(sz) (see kernel/vm.c), which for a
      // per-phdr-ascending-vaddr ELF (what every real toolchain
      // produces) lands exactly at the start of whichever page this
      // segment's vaddr falls within.
      if((sz = allocuvm(pgdir, sz, interp_bias + iph.vaddr + iph.memsz)) == 0)
        goto bad;
      if(loaduvm(pgdir, (char*)(interp_bias + iph.vaddr), iip, iph.off, iph.filesz) < 0)
        goto bad;
    }
    interp_entry = interp_bias + ielf.entry;
    iunlockput(iip);
    end_op();
    iip = 0;
  }

  // See the non-interpreter path above for USTACKPAGES's own comment.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + (USTACKPAGES+1)*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - (USTACKPAGES+1)*PGSIZE));
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
  // for the *main executable* today (a PT_INTERP interpreter, if any,
  // is ET_DYN and gets a real interp_bias - see AT_BASE below - but
  // AT_PHDR/AT_PHENT/AT_PHNUM always describe the main executable's
  // own program headers, never the interpreter's, exactly like a real
  // Linux kernel's ELF loader).
  AUXENT(AT_PHDR, elf.phoff);
  AUXENT(AT_PHENT, elf.phentsize);
  AUXENT(AT_PHNUM, elf.phnum);
  AUXENT(AT_PAGESZ, PGSIZE);
  AUXENT(AT_BASE, has_interp ? interp_bias : 0);
  AUXENT(AT_ENTRY, elf.entry);
  // Deliberately always 0/0/0/0/false here, even for a setuid/setgid
  // exec (su_uid/su_gid set above) where real uid/gid now genuinely
  // differ from effective: musl's __libc_start_main
  // (musl/src/env/__libc_start_main.c) takes its "secure exec" path -
  // AT_UID!=AT_EUID || AT_GID!=AT_EGID || AT_SECURE - by calling
  // SYS_ppoll, which has no dispatch table entry at all (kernel/
  // syscall.c, include/syscall.h's own SYS_ppoll comment already
  // documents this as intentionally unreachable) and crashes any
  // setuid-exec'd program outright. The real privilege drop still
  // happens correctly (curproc->euid/egid/suid/sgid, set at the Commit
  // section below, are what kernel/fs.c's permcheck() actually checks) -
  // this only affects what userland's own auxv introspection sees, and
  // reporting uid==euid here is what keeps su/login's own musl startup
  // from taking a path this port doesn't support yet.
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
  // Jump into the interpreter, if there is one, instead of straight to
  // the main executable - it's the interpreter that reads AT_ENTRY
  // back out of the auxv above and transfers control to the real
  // entry point itself, once it's done relocating/mapping everything
  // the main executable actually needs (see musl/ldso/dynlink.c).
  curproc->tf->eip = has_interp ? interp_entry : elf.entry;
  curproc->tf->esp = sp;
  if(su_uid >= 0)
    curproc->euid = curproc->suid = su_uid;
  if(su_gid >= 0)
    curproc->egid = curproc->sgid = su_gid;
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
  if(iip){
    iunlockput(iip);
    end_op();
  }
  return -1;
}
