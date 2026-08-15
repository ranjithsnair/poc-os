// Kernel-side termios/ioctl definitions for the console device. Not
// derived from musl's headers (this is kernel code, built with its own
// -m64 -ffreestanding flags, not MUSL_INC) - but struct termios/winsize
// and the TCGETS/TCSETS/TIOCGWINSZ request numbers below must match
// musl's musl/arch/generic/bits/{ioctl.h,termios.h} and
// musl/include/alltypes.h.in field-for-field and value-for-value, since
// sys_ioctl() (kernel/sysproc.c) bit-copies these structs straight into
// a musl-linked userspace program's own struct termios/winsize.

// musl/arch/generic/bits/ioctl.h (x86_64 has no arch-specific override,
// so these generic values are what musl's ioctl()/tcgetattr()/
// tcsetattr()/tcgetwinsize() actually issue).
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

// c_lflag bits sys_ioctl() inspects (musl/arch/generic/bits/termios.h).
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010

// c_cc[] indices sys_ioctl()/consoleread() inspect (same musl header -
// VEOF/VTIME/VMIN are the only three this console gives real meaning
// to; the rest of c_cc[] round-trips through TCGETS/TCSETS unexamined).
#define VEOF  4
#define VTIME 5
#define VMIN  6

#define NCCS 32

// Field layout must match musl/arch/generic/bits/termios.h exactly -
// same compiler/arch on both sides, so identical field order/types
// gives identical struct padding without hand-placed pad bytes.
struct termios {
  uint c_iflag;
  uint c_oflag;
  uint c_cflag;
  uint c_lflag;
  uchar c_line;
  uchar c_cc[NCCS];
  uint __c_ispeed;
  uint __c_ospeed;
};

// musl/include/alltypes.h.in's __NEED_struct_winsize.
struct winsize {
  ushort ws_row;
  ushort ws_col;
  ushort ws_xpixel;
  ushort ws_ypixel;
};
