// Memory layout

#define EXTMEM  0x100000            // Start of extended memory
#define PHYSTOP 0xE000000           // Top physical memory
#define DEVSPACE 0xFE000000         // Other devices are at high addresses

// RAMDISK_PADDR/RAMDISK_SIZE: the whole root filesystem (fs.img, see
// mkfs/mkfs.c and param.h's FSSIZE) is loaded into RAM at this fixed
// physical address by the boot loader, before the kernel ever runs -
// kernel/ide.c's iderw() just memmove()s to/from it instead of talking
// to a real disk controller (see that file's own comment for why: no
// real disk driver poc-os has - raw ATA PIO, real AHCI, or a real USB
// Mass Storage stack - works across every one of real hardware/
// VirtualBox/QEMU with every controller/boot-media combination the way
// the *firmware's own* boot-time disk driver already does, so the
// loader leans on that instead of this kernel owning one itself).
// RAMDISK_SIZE is a literal, not FSSIZE*BSIZE: this header is included
// by boot/*.asm too (via the Makefile's cpp-then-nasm pipeline), kept
// deliberately dependency-free of param.h/fs.h, so the two are spelled
// out and cross-checked against each other by a _Static_assert in
// kernel/ide.c instead. 4MB leaves comfortable room below it for the
// kernel's own footprint (loaded at EXTMEM above, realistically under
// 1MB) with margin, and the boot loader hardcodes the same address.
#define RAMDISK_PADDR 0x400000
#define RAMDISK_SIZE  2048000       // FSSIZE(4000) * BSIZE(512), param.h/fs.h

// Key addresses for address space layout (see kmap in vm.c for layout)
//
// KERNBASE is a canonical higher-half address (top -2GB, the same
// shape Linux uses) - PHYSTOP is modest (~224MB, above) so it, plus
// DEVSPACE, fits comfortably in the 2GB above KERNBASE without needing
// a separate physical direct-map region: V2P/P2V stay a plain offset
// by KERNBASE.
#define KERNBASE 0xFFFFFFFF80000000
#define KERNLINK (KERNBASE+EXTMEM)  // Address where kernel is linked

#define V2P(a) (((uintp) (a)) - KERNBASE)
#define P2V(a) ((void *)(((uintp) (a)) + KERNBASE))

#define V2P_WO(x) ((x) - KERNBASE)    // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE)    // same as P2V, but without casts
