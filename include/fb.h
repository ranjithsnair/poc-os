// Framebuffer ioctl - poc-os's own ABI (unlike include/termios.h's
// TCGETS/TIOCGWINSZ, this has no musl-side numeric value it has to
// match, since nothing in musl's own headers issues a framebuffer
// ioctl at all - so the request number and struct fb_info layout below
// are free to be whatever's convenient here). Loosely mirrors the
// spirit of Linux's real fbdev FBIOGET_VSCREENINFO (same idea: "give
// me the current mode's geometry/pixel format before I mmap it"), not
// its exact struct - a userspace program discovers the framebuffer's
// width/height/pitch/bpp/RGB layout this way instead of guessing.
// Shared by kernel/sysproc.c's sys_ioctl() and any userspace caller
// (e.g. bash/poc/fbtest.c).

#define FBIOGET_VSCREENINFO 1

struct fb_info {
  uint xres;
  uint yres;
  uint pitch;         // bytes per scanline
  uchar bpp;
  uchar red_mask_size,   red_field_pos;
  uchar green_mask_size, green_field_pos;
  uchar blue_mask_size,  blue_field_pos;
};
