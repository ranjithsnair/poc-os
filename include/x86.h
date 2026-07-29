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
uint rcr2(void);
void lcr3(uint val);

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
