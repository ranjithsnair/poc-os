// Boot loader, stage 1.
//
// Part of the boot block, along with bootasm.asm, which calls
// bootmain(). bootasm.asm has put the processor into protected 32-bit
// mode. This file's only job is to load stage 2 (boot/boot2main.c,
// which does the real work: loading the kernel and the whole root
// filesystem image - see its own comment) from disk into RAM and jump
// to it.
//
// Kept this small deliberately, not just incidentally: this file,
// linked together with bootasm.asm, has to fit in the 510-byte boot
// sector (see boot/sign.pl) - there's no room here for the ELF-parsing
// and multi-drive logic stage 2 needs, which is exactly why stage 2
// exists as a separate, unconstrained-size stage instead of all of
// this just living in one file the way the original xv6 bootloader
// this is descended from did.

#include "types.h"
#include "bootx86.h"

#define SECTSIZE       512
#define STAGE2_ADDR    0x10000
// STAGE2_SECTORS: how many sectors after this boot sector hold stage
// 2 - see the Makefine's own BOOT2_MAX_SECTORS check, which fails the
// build if boot2main.c's actual compiled size ever exceeds this.
#define STAGE2_SECTORS 256

static void waitdisk(void);
static void readsect(void *dst, uint offset);

void
bootmain(void)
{
  uchar *dst;
  uint i;

  dst = (uchar*)STAGE2_ADDR;
  for(i = 0; i < STAGE2_SECTORS; i++)
    readsect(dst + i*SECTSIZE, 1 + i);

  ((void(*)(void))STAGE2_ADDR)();
}

static void
waitdisk(void)
{
  while((inb(0x1F7) & 0xC0) != 0x40)
    ;
}

// Read a single sector at offset (LBA) from drive 0 (the boot drive -
// stage 2 and the kernel both live on it) into dst.
static void
readsect(void *dst, uint offset)
{
  waitdisk();
  outb(0x1F2, 1);   // count = 1
  outb(0x1F3, offset);
  outb(0x1F4, offset >> 8);
  outb(0x1F5, offset >> 16);
  outb(0x1F6, (offset >> 24) | 0xE0);
  outb(0x1F7, 0x20);  // cmd 0x20 - read sectors

  waitdisk();
  insl(0x1F0, dst, SECTSIZE/4);
}
