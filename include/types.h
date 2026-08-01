typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

// uint64: always exactly 8 bytes, on every build, regardless of which
// compiler/mode is doing the compiling - unlike uintp below, this isn't
// tied to native pointer width. `unsigned long` doesn't work for this:
// it's 8 bytes under an LP64 64-bit compile but only 4 under a 32-bit
// (ILP32) one, whereas `unsigned long long` is 8 bytes under GCC on
// both. This matters because boot/bootmain.c - always compiled 32-bit,
// see the Makefile's BOOTCC, since it runs before long mode exists -
// still has to parse a 64-bit kernel's genuinely-64-bit-field ELF
// header (include/elf.h's X64 struct elfhdr/proghdr) when ARCH=64.
typedef unsigned long long uint64;

// uintp: an unsigned integer the same width as a pointer on this build -
// unsigned int (4 bytes) under the 32-bit build, an exact alias for uint
// so 32-bit codegen is unaffected; unsigned long (8 bytes) under the
// 64-bit build. Used anywhere a virtual or physical address is stored in
// an integer (not a typed pointer) - e.g. memlayout.h's V2P/P2V - since a
// plain uint would truncate a 64-bit address.
#ifdef X64
typedef unsigned long  uintp;
#else
typedef unsigned int   uintp;
#endif

typedef uintp pde_t;
