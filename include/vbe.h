// Boot-time VBE (VESA BIOS Extensions) linear-framebuffer info, handed
// off from real mode to the kernel. C-only (unlike include/memlayout.h,
// this is never %included by boot/boot2_bios.asm's real-mode assembly -
// NASM has no idea what to do with a C `struct` - see VBE_INFO_MAGIC's
// own comment there for why the two constants both sides actually
// share live in memlayout.h instead). boot2_bios.asm's setup_vbe
// writes this exact byte layout using raw hardcoded offsets (0x4000,
// 0x4004, 0x4008, ...) rather than a shared symbolic header, the same
// asm/C decoupling include/memlayout.h's RAMDISK_SIZE comment already
// documents - keep the two in sync by hand if this struct ever changes.
//
// Fields mirror the subset of the VBE 2.0+ ModeInfoBlock that matters
// for a linear-framebuffer driver - see kernel/vbe.c's vbeinit() for
// what actually reads and validates this.
struct vbeinfo {
  uint magic;               // 0x00 - VBE_INFO_MAGIC iff the probe succeeded
  uint phys_base;           // 0x04 - PhysBasePtr: physical addr of the LFB
  uint pitch;                // 0x08 - BytesPerScanLine
  uint xres;                 // 0x0C
  uint yres;                 // 0x10
  uchar bpp;                  // 0x14 - BitsPerPixel (32)
  uchar red_mask_size;         // 0x15
  uchar red_field_pos;         // 0x16
  uchar green_mask_size;       // 0x17
  uchar green_field_pos;       // 0x18
  uchar blue_mask_size;        // 0x19
  uchar blue_field_pos;        // 0x1A
  uchar reserved;              // 0x1B - pad to 28 bytes
} __attribute__((packed));
