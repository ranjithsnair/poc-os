/*
 * Not part of mlibc itself (only ships under the glibc option, which we
 * don't enable -- see meson.build's doc comment, same reasoning as
 * sys/ioctl.h/paths.h). busybox's libbb.h includes <sys/sysmacros.h>
 * unconditionally whenever `major` isn't already a macro, the way it
 * would on any real Unix, purely for the major()/minor()/makedev() dev_t
 * packing macros -- this kernel has no real device nodes (no mknod
 * applet is enabled -- see toolchain/busybox-pocos.config), so nothing
 * here ever needs to round-trip through an actual device driver; the
 * bit layout only needs to be internally consistent with itself.
 */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#define major(dev) ((int)(((unsigned long)(dev) >> 8) & 0xff))
#define minor(dev) ((int)((unsigned long)(dev) & 0xff))
#define makedev(maj, min) ((unsigned long)(((maj) & 0xff) << 8) | ((min) & 0xff))

#endif /* _SYS_SYSMACROS_H */
