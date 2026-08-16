// Boot loader, stage 2.
//
// Loaded into RAM and entered by boot/bootmain.c (stage 1), already in
// 32-bit protected mode with a flat, identity-mapped GDT (bootasm.asm
// set that up before stage 1 even ran). boot/boot2asm.asm's tiny stub
// (guaranteed to be the first thing at STAGE2_ADDR - see its own
// comment) calls this file's start(). Loads the kernel ELF image from
// disk (starting right after this stage's own sectors - see
// STAGE2_SECTORS in boot/bootmain.c), then loads the *entire* root
// filesystem image (fs.img, drive 1) into a fixed RAM address
// (RAMDISK_PADDR, memlayout.h) so the kernel never has to touch disk
// hardware itself (see kernel/ide.c's own comment for the full
// reasoning), then jumps to the kernel's entry point.
//
// This ATA-PIO path (readsect_drive()/readseg() below) only works
// against a real/emulated legacy IDE controller - QEMU/VirtualBox with
// the disk attached as an IDE hard disk. A real-hardware/AHCI/USB-boot
// equivalent stage 1+2 pair, using BIOS INT13h extended reads instead
// (the one disk-reading mechanism real firmware already implements
// correctly for every one of those cases), is the separate path that
// covers those; this file is the QEMU/VirtualBox-as-IDE-disk path, and
// also defines the ramdisk contract (kernel/ide.c) both paths hand off
// to identically.

#include "types.h"
#include "elf.h"
#include "memlayout.h"
#include "bootx86.h"

#define SECTSIZE  512

// Must match boot/bootmain.c's own STAGE2_SECTORS - where the kernel
// image starts, in sectors from the start of the disk.
#define KERNEL_LBA 257

static void waitdisk(void);
static void readsect_drive(void *dst, uint offset, int drive);
static void readseg(uchar *pa, uint count, uint offset);

void
start(void)
{
  struct elfhdr *elf;
  struct proghdr *ph, *eph;
  void (*entry)(void);
  uchar *pa;
  uint i;

  // Scratch space for the ELF header/program headers only - not the
  // segment *data* itself (that goes straight to ph->paddr below).
  // Must avoid both stage 2's own code (loaded at 0x10000, up to
  // STAGE2_SECTORS*512 = 0x20000 bytes - boot/bootmain.c) and the
  // kernel's real target load address (EXTMEM, 0x100000 - memlayout.h/
  // kernel64.ld) that the segment-loading loop below writes to;
  // 0x40000 sits comfortably clear of both.
  elf = (struct elfhdr*)0x40000;

  // Read 1st page of the kernel ELF off disk.
  readseg((uchar*)elf, 4096, 0);

  if(elf->magic != ELF_MAGIC)
    return;  // nothing sensible to do - freeze (see boot/boot2asm.asm's spin2)

  // Load each program segment (ignores ph flags) - see boot/bootmain.c's
  // old version of this same loop for why ph->paddr, not ph->vaddr.
  ph = (struct proghdr*)((uchar*)elf + (uint)elf->phoff);
  eph = ph + elf->phnum;
  for(; ph < eph; ph++){
    pa = (uchar*)(uint)ph->paddr;
    readseg(pa, (uint)ph->filesz, (uint)ph->off);
    if(ph->memsz > ph->filesz)
      stosb(pa + (uint)ph->filesz, 0, (uint)(ph->memsz - ph->filesz));
  }

  // Load the whole root filesystem image (drive 1 - see kernel/ide.c's
  // own dev&1 convention) into the fixed ramdisk address every
  // subsequent iderw() call assumes it's already at. RAMDISK_SIZE is
  // in bytes; drive sectors are SECTSIZE(512) each, matching BSIZE, so
  // this is exactly RAMDISK_SIZE/SECTSIZE whole sectors, no partial
  // final sector to special-case.
  for(i = 0; i < RAMDISK_SIZE/SECTSIZE; i++)
    readsect_drive((uchar*)RAMDISK_PADDR + i*SECTSIZE, i, 1);

  // Call the entry point from the ELF header. Does not return!
  entry = (void(*)(void))(uint)elf->entry;
  entry();
}

static void
waitdisk(void)
{
  while((inb(0x1F7) & 0xC0) != 0x40)
    ;
}

// Read a single sector at offset (LBA, within the given drive: 0 -
// primary/master, the disk this stage and the kernel both live on; 1 -
// secondary/slave, the root filesystem image - same bit kernel/ide.c's
// idestart() used to OR into 0x1f6 for the exact same reason) into dst.
static void
readsect_drive(void *dst, uint offset, int drive)
{
  waitdisk();
  outb(0x1F2, 1);   // count = 1
  outb(0x1F3, offset);
  outb(0x1F4, offset >> 8);
  outb(0x1F5, offset >> 16);
  outb(0x1F6, (offset >> 24) | 0xE0 | ((drive&1)<<4));
  outb(0x1F7, 0x20);  // cmd 0x20 - read sectors

  waitdisk();
  insl(0x1F0, dst, SECTSIZE/4);
}

// Read 'count' bytes at 'offset' from the kernel image (drive 0,
// starting at KERNEL_LBA) into physical address 'pa'. Might copy more
// than asked.
static void
readseg(uchar *pa, uint count, uint offset)
{
  uchar *epa;

  epa = pa + count;

  // Round down to sector boundary.
  pa -= offset % SECTSIZE;

  // Translate from bytes to sectors; the kernel image starts at
  // KERNEL_LBA.
  offset = (offset / SECTSIZE) + KERNEL_LBA;

  for(; pa < epa; pa += SECTSIZE, offset++)
    readsect_drive(pa, offset, 0);
}
