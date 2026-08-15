// Kernel entry point (reached from kernel/entry.asm): main() runs on
// the boot CPU and initializes every subsystem in dependency order.
// startothers() (AP/multiprocessor bring-up) is currently a stub - see
// its own comment - so poc runs single-CPU for now.

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

// Start the non-boot (AP) processors.
static void
startothers(void)
{
  // SMP bring-up isn't implemented yet: APs need the same real-mode ->
  // protected-mode -> long-mode transition kernel/entry.asm does for
  // the boot processor, which kernel/entryother.asm (never leaves
  // 32-bit protected mode - see the Makefile) doesn't do. Every CPU
  // past cpu0 just stays parked; poc runs single-CPU here.
  return;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

