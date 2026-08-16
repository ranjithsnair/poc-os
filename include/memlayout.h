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

// VBE_INFO_PADDR: where boot/boot2_bios.asm's real-mode VBE probe (see
// its own comment) parks the struct vbeinfo (include/vbe.h) it builds -
// read back by kernel/vbe.c's vbeinit() via P2V(). Physical 0x4000 sits
// in the gap between stage 2's own code (0x1000-0x3000,
// boot/bootasm_bios.asm's STAGE2_OFF/STAGE2_SECTORS) and its stack
// (grows down from 0x9000, boot/boot2_bios.asm) - clear of the IVT
// (0-0x3FF), the BDA (0x400-0x4FF), and the AP-trampoline range
// (0x7000+) kernel/main.c's startothers() doesn't write until well
// after the kernel has already read this out. 0x4100/0x4300 (also in
// this gap) are transient real-mode scratch buffers for the VBE probe
// itself - never read by the kernel.
#define VBE_INFO_PADDR 0x4000

// Written by boot/boot2_bios.asm's setup_vbe *last*, only once "Set VBE
// Mode" has actually reported success - see include/vbe.h's struct
// vbeinfo and kernel/vbe.c's vbeinit() for the C side that reads this
// back and what an unwritten/mismatched magic means (graceful degrade
// to text-only console, not a boot failure). A #define here rather
// than in vbe.h itself: this header, unlike vbe.h, is included by both
// C and boot/boot2_bios.asm's real-mode assembly (via the Makefile's
// cpp-then-nasm pipeline) - vbe.h's C struct syntax would fail to
// assemble if NASM ever saw it, so only the plain macros both sides
// need to agree on live here, same reasoning as RAMDISK_PADDR/
// RAMDISK_SIZE just above.
#define VBE_INFO_MAGIC 0x31454256   // "VBE1" (little-endian dword)

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
