// Routines to let C code use special x86 instructions.
//
// These are implemented in kernel/x86.asm (NASM), not as inline asm here,
// so they are ordinary extern function declarations. See kernel/x86.asm
// for the actual instruction sequences and calling-convention notes.

uchar inb(ushort port);
void insl(int port, void *addr, int cnt);
void outb(ushort port, uchar data);
void outw(ushort port, ushort data);
void outsl(int port, const void *addr, int cnt);
void stosb(void *addr, int data, int cnt);
void stosl(void *addr, int data, int cnt);

struct segdesc;
void lgdt(struct segdesc *p, int size);

struct gatedesc;
void lidt(struct gatedesc *p, int size);

void ltr(ushort sel);
uint readeflags(void);
void loadgs(ushort v);
void cli(void);
void sti(void);
uint xchg(volatile uint *addr, uint newval);
void clearlock(volatile uint *p);
// CR2 (the faulting address on a page fault) and the argument to lcr3
// (a page table's physical address) are pointer-width, not fixed 32-bit -
// on the 64-bit build a plain uint here would silently truncate a
// canonical high address.
uintp rcr2(void);
void lcr3(uintp val);

#ifdef X64
// WRMSR is only ever used here for MSR_FS_BASE (see include/mmu.h),
// which takes a full 64-bit value - unlike lgdt/lidt's descriptor-table
// registers, there's no 32-bit build counterpart that would need this
// under a different implementation, so it's declared X64-only rather
// than unconditionally like rcr2/lcr3 above.
void wrmsr(uint msr, uint64 val);
#endif

#ifdef X64
//PAGEBREAK: 36
// Layout of the trap frame built on the stack by the hardware and by
// kernel/trapasm64.asm, and passed to trap(). Long mode has no pusha, so
// trapasm64.asm pushes registers one at a time in a chosen order (see the
// comment there) - the fields below are listed in reverse of that push
// order (last pushed = lowest address = first field), the same
// low-to-high-address convention the 32-bit trapframe below uses.
// ds/es/fs/gs are dropped entirely (unlike the 32-bit trapframe): once
// segmentation is flat, as it is here, reloading them on every trap has
// no effect and saving them is pointless.
//
// eip/esp/eflags/eax keep their 32-bit names (rather than rip/rsp/
// rflags/rax) even though they're now uint64 - trap.c, proc.c, exec.c,
// and syscall.c reference these fields by name without needing an
// #ifdef at every call site; only their width changes, and callers
// that cast into/out of them already use the arch-neutral uintp type
// for exactly this reason.
//
// Unlike the 32-bit trapframe below, there's no ds/es/fs/gs here at
// all - not even as inert padding. That was tried and is exactly
// wrong: sizeof(this struct) has to equal precisely what
// trapasm64.asm's alltraps actually pushes (this struct's layout is
// its contract), because whole-struct copies of *p->tf (fork()'s
// `*np->tf = *curproc->tf`, in particular) read/write exactly
// sizeof(struct trapframe) bytes starting at p->tf - which for a real
// (not proc.c's userinit()-constructed fake initial one) trapframe
// sits flush against the top of the process's one-page kernel stack.
// Padding this struct out with 32 unused trailing bytes made every
// such copy read 32 bytes past the end of that page.
struct trapframe {
  uint64 r15;
  uint64 r14;
  uint64 r13;
  uint64 r12;
  uint64 r11;
  uint64 r10;
  uint64 r9;
  uint64 r8;
  uint64 rbp;
  uint64 rdi;
  uint64 rsi;
  uint64 rdx;
  uint64 rcx;
  uint64 rbx;
  uint64 eax;

  uint64 trapno;

  // below here defined by x86-64 hardware
  uint64 err;
  uint64 eip;
  uint64 cs;
  uint64 eflags;

  // below here only when crossing rings, such as from user to kernel
  uint64 esp;
  uint64 ss;
};

#else
//PAGEBREAK: 36
// Layout of the trap frame built on the stack by the
// hardware and by trapasm.asm, and passed to trap().
struct trapframe {
  // registers as pushed by pusha
  uint edi;
  uint esi;
  uint ebp;
  uint oesp;      // useless & ignored
  uint ebx;
  uint edx;
  uint ecx;
  uint eax;

  // rest of trap frame
  ushort gs;
  ushort padding1;
  ushort fs;
  ushort padding2;
  ushort es;
  ushort padding3;
  ushort ds;
  ushort padding4;
  uint trapno;

  // below here defined by x86 hardware
  uint err;
  uint eip;
  ushort cs;
  ushort padding5;
  uint eflags;

  // below here only when crossing rings, such as from user to kernel
  uint esp;
  ushort ss;
  ushort padding6;
};
#endif
