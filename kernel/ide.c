// Ramdisk-backed block "device" - stands in for a real disk driver.
//
// poc-os has no real disk driver of any kind: not the raw ATA PIO this
// file used to be (only ever worked against a real/emulated legacy IDE
// controller - not AHCI-mode SATA, not a USB Mass Storage device, and
// not the ATAPI protocol a virtual/real CD or DVD drive actually
// speaks), and not a real AHCI or USB stack either (both genuinely
// large undertakings - PCI enumeration and command-queue management
// for AHCI, a whole USB stack for Mass Storage - that would still only
// cover *some* of real hardware/VirtualBox/QEMU, not all of them
// uniformly).
//
// Instead, the boot loader (boot/bootmain.c on the QEMU/VirtualBox-as-
// IDE-disk path; boot/boot2.asm on the real-hardware/AHCI/USB-boot
// path - see that file's own comment) loads the *entire* root
// filesystem image (fs.img, RAMDISK_SIZE bytes - see memlayout.h) into
// RAM at a fixed physical address, RAMDISK_PADDR, before the kernel
// ever starts running - using whichever disk-reading mechanism that
// specific boot path has (real ATA PIO, or BIOS INT13h extended reads,
// which is itself really just calling out to *firmware's own* disk
// driver, already written and already correct for whatever controller
// a given machine actually has). Once the kernel is running, it never
// touches disk hardware again: every iderw() call below is just a
// memmove() to/from that already-loaded RAM image. Writes (mkdir, rm,
// mv, ...) modify the RAM copy only - like any live-boot/initrd-style
// system, changes don't persist across a reboot unless something
// explicitly writes the RAM image back out, which nothing here does.
//
// ideinit()/ideintr() are kept as no-op stubs, not removed outright,
// so kernel/main.c and kernel/trap.c don't need call-site changes.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

_Static_assert(RAMDISK_SIZE == FSSIZE * BSIZE,
               "RAMDISK_SIZE (memlayout.h) must match FSSIZE*BSIZE (param.h/fs.h) - "
               "the boot loader and mkfs both size fs.img by the latter.");

void
ideinit(void)
{
  // Nothing to initialize - no real disk controller involved.
}

void
ideintr(void)
{
  // The ramdisk never raises an interrupt; iderw() below is already
  // synchronous. Kept only because kernel/trap.c's IRQ_IDE case still
  // calls it - a no-op body is the correct response to an interrupt
  // that (on real hardware particularly) should never actually fire
  // for a device this kernel never programs.
}

// Sync buf with the ramdisk. If B_DIRTY is set, copy buf->data into
// the ramdisk and clear B_DIRTY; else if B_VALID is not set, copy from
// the ramdisk into buf->data and set B_VALID - the exact same
// contract the real iderw() this replaces had, so kernel/bio.c's
// bread()/bwrite() (the only callers) needed no changes at all.
void
iderw(struct buf *b)
{
  char *disk;

  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(b->blockno >= FSSIZE)
    panic("iderw: blockno out of range");

  disk = (char*)P2V(RAMDISK_PADDR) + (uintp)b->blockno * BSIZE;
  if(b->flags & B_DIRTY){
    memmove(disk, b->data, BSIZE);
    b->flags &= ~B_DIRTY;
  } else {
    memmove(b->data, disk, BSIZE);
  }
  b->flags |= B_VALID;
}
