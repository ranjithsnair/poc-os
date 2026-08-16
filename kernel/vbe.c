// VBE linear-framebuffer info: reads back what boot/boot2_bios.asm's
// real-mode setup_vbe probe left at VBE_INFO_PADDR (include/
// memlayout.h) and, once validated, exposes it as the kernel-global
// `vbe` every other framebuffer-facing piece (kernel/sysproc.c's
// sys_mmap()/sys_ioctl() FRAMEBUFFER paths) reads from.

#include "types.h"
#include "defs.h"
#include "memlayout.h"
#include "vbe.h"

struct vbeinfo vbe;

void
vbeinit(void)
{
  struct vbeinfo *boot = (struct vbeinfo*)P2V(VBE_INFO_PADDR);

  vbe = *boot;

  // Validate more than just the magic - a real, sane mode rather than
  // whatever happened to be sitting at VBE_INFO_PADDR (BIOS POST
  // doesn't guarantee unused low memory is zeroed). phys_base >=
  // PHYSTOP is expected for any real VBE/Bochs-VGA linear framebuffer
  // (well above this kernel's own RAM pool, see kernel/kalloc.c's own
  // PHYSTOP bound) - anything below it would indicate garbage, not a
  // real framebuffer.
  if(vbe.magic != VBE_INFO_MAGIC || vbe.bpp != 32 ||
     vbe.xres == 0 || vbe.yres == 0 || vbe.pitch == 0 ||
     vbe.phys_base < PHYSTOP){
    vbe.magic = 0;
    cprintf("vbeinit: no usable VBE framebuffer - text console only\n");
    return;
  }

  cprintf("vbeinit: %dx%d %dbpp framebuffer at 0x%x (pitch %d)\n",
          vbe.xres, vbe.yres, vbe.bpp, vbe.phys_base, vbe.pitch);
}
