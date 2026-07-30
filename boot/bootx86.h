// A handful of x86 instruction wrappers, kept as GCC inline asm (rather
// than real calls into kernel/x86.asm, as the rest of the kernel and user
// space use - see include/x86.h) only for boot/bootmain.c.
//
// The boot sector has a hard 510-byte budget (512 minus the 2-byte 0x55AA
// signature). Making these real out-of-line calls adds enough call
// overhead, on both sides, to blow that budget; inlining them keeps
// bootmain.c small enough to fit.

static inline uchar
inb(ushort port)
{
  uchar data;

  asm volatile("in %1,%0" : "=a" (data) : "d" (port));
  return data;
}

static inline void
insl(int port, void *addr, int cnt)
{
  asm volatile("cld; rep insl" :
               "=D" (addr), "=c" (cnt) :
               "d" (port), "0" (addr), "1" (cnt) :
               "memory", "cc");
}

static inline void
outb(ushort port, uchar data)
{
  asm volatile("out %0,%1" : : "a" (data), "d" (port));
}

static inline void
stosb(void *addr, int data, int cnt)
{
  asm volatile("cld; rep stosb" :
               "=D" (addr), "=c" (cnt) :
               "0" (addr), "1" (cnt), "a" (data) :
               "memory", "cc");
}
