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
// still has to parse the 64-bit kernel's genuinely-64-bit-field ELF
// header (include/elf.h's struct elfhdr/proghdr).
typedef unsigned long long uint64;

// uintp: an unsigned integer the same width as a pointer on this build.
// Used anywhere a virtual or physical address is stored in an integer
// (not a typed pointer) - e.g. memlayout.h's V2P/P2V - since a plain
// uint would truncate a 64-bit address. Deliberately `unsigned long`,
// not a fixed-width type: its size already follows the target ABI
// (8 bytes under the kernel/user -m64 build's LP64 model, 4 under
// boot/*.c's own always-32-bit -m32 build's ILP32 model - see the
// Makefile's BOOTCFLAGS - which is exactly the width each of those
// needs its own native pointers to be), with no preprocessor
// conditional required to pick between them.
typedef unsigned long  uintp;

typedef uintp pde_t;
