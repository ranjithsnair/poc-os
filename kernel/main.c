// Kernel entry point (reached from kernel/entry.asm) and multiprocessor
// bring-up: main() runs on the boot CPU and initializes every subsystem
// in dependency order, then wakes the other CPUs (startothers()), each
// of which lands in mpenter() below and runs its own much shorter
// per-CPU setup (mpmain()).

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void startothers(void);
static void mpmain(void)  __attribute__((noreturn));
extern pde_t *kpgdir;
extern char end[]; // first address after kernel loaded from ELF file

// Bootstrap processor starts running C code here.
// Allocate a real stack and switch to it, first
// doing some setup required for memory allocator to work.
int
main(void)
{
  kinit1(end, P2V(4*1024*1024)); // phys page allocator
  kvmalloc();      // kernel page table
  mpinit();        // detect other processors
  lapicinit();     // interrupt controller
  seginit();       // segment descriptors
  picinit();       // disable pic
  ioapicinit();    // another interrupt controller
  consoleinit();   // console hardware
  uartinit();      // serial port
  vbeinit();       // linear framebuffer, if boot/boot2_bios.asm found one -
                   // after uartinit() so its result line reaches serial too
  pinit();         // process table
  tvinit();        // trap vectors
  binit();         // buffer cache
  fileinit();      // file table
  ideinit();       // disk
  startothers();   // start other processors
  // Starts right past the ramdisk (RAMDISK_PADDR..RAMDISK_PADDR+
  // RAMDISK_SIZE, memlayout.h - kernel/ide.c's whole backing store),
  // not at a flat 4MB like the original xv6 kinit2 call this replaces:
  // that pre-loaded fs.img image has to stay put for the ramdisk's
  // entire lifetime, so its pages can never be handed out as ordinary
  // free memory the way anything at a bare 4MB boundary otherwise
  // would be.
  kinit2(P2V(RAMDISK_PADDR + RAMDISK_SIZE), P2V(PHYSTOP)); // must come after startothers()
  userinit();      // first user process
  mpmain();        // finish this processor's setup
}

// Common CPU setup code.
static void
mpmain(void)
{
  cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
  idtinit();       // load idt register
  xchg(&(mycpu()->started), 1); // tell startothers() we're up
  scheduler();     // start running processes
}

// Other CPUs jump here (in 64-bit mode, courtesy of kernel/entryother.asm)
// once startothers() below wakes them up.
static void
mpenter(void)
{
  switchkvm();
  seginit();
  lapicinit();
  mpmain();
}

// The boot-time page table kernel/entry.asm built, for use in
// startothers() below.
extern pde_t entrypml4[];

// Start the non-boot (AP) processors.
static void
startothers(void)
{
  extern uchar _binary_build_entryother_start[], _binary_build_entryother_size[];
  uchar *code;
  struct cpu *c;
  char *stack;

  // Write entry code to unused memory at 0x7000. The linker has placed
  // the image of entryother.asm in _binary_build_entryother_start (the
  // symbol name is derived from the build/entryother path given to
  // ld's -b binary option).
  code = P2V(0x7000);
  memmove(code, _binary_build_entryother_start, (uint64)_binary_build_entryother_size);

  for(c = cpus; c < cpus+ncpu; c++){
    if(c == mycpu())  // We've started already.
      continue;

    // Tell entryother.asm what stack to use, where to enter, and what
    // pgdir to use. We cannot use kpgdir yet, because the AP processor
    // is running in low memory, so we use entrypml4 for the APs too -
    // see the comment atop kernel/entryother.asm.
    stack = kalloc();
    *(uint64*)(code-8)  = (uint64)stack + KSTACKSIZE;
    *(uint64*)(code-16) = (uint64)mpenter;
    *(uint64*)(code-24) = (uint64)V2P(entrypml4);

    lapicstartap(c->apicid, (uint)V2P(code));

    // wait for cpu to finish mpmain()
    while(c->started == 0)
      ;
  }
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

