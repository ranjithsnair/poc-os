/*
 * Not part of mlibc itself (its sys/ioctl.h only ships under the glibc
 * option, which we don't enable -- see meson.build's doc comment). bash's
 * jobs.c includes <sys/ioctl.h> unconditionally, the way it would on any
 * real Unix, so this just needs to exist and declare ioctl() -- the
 * actual request codes below are dummy placeholders (window-size
 * queries), since this kernel has no PTY/window concept yet. Real
 * request codes bash cares about for I/O (TCGETS/TCSETS-equivalents) go
 * through <termios.h> instead, which abi-bits/termios.h + mlibc's own
 * posix layer already cover.
 */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <bits/winsize.h>

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif // _SYS_IOCTL_H
