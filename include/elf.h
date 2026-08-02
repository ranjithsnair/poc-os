// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian

// File header
// entry is where execution starts (used by both boot/bootmain.c, to
// jump into the kernel, and exec(), to set the new process's eip);
// phoff/phnum locate the array of proghdr entries below, which is what
// both of those actually load into memory.
//
// The 64-bit ELF header/program-header shapes below aren't just the
// 32-bit ones with wider fields - a couple of fields move, to keep every
// field naturally aligned (notably p_flags, which is the *second* field
// of Elf64_Phdr instead of the second-to-last). boot/bootmain.c parses
// whichever shape matches the kernel it was built to load - it stays a
// 32-bit program either way (see the Makefile's BOOTCC), but reads a
// 64-bit-shaped header when ARCH=64, since that's the shape the kernel
// it's loading was linked with.
#ifdef X64
struct elfhdr {
  uint magic;  // must equal ELF_MAGIC
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
} __attribute__((packed));

// Program section header: one loadable segment (only entries with
// type == ELF_PROG_LOAD are used - see exec.c). filesz bytes are read
// from the file at off; if memsz > filesz the remainder (typically
// .bss) is left zeroed, since allocuvm() zero-fills new pages.
struct proghdr {
  uint type;
  uint flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
} __attribute__((packed));

#else
struct elfhdr {
  uint magic;  // must equal ELF_MAGIC
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint entry;
  uint phoff;
  uint shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
};

// Program section header: one loadable segment (only entries with
// type == ELF_PROG_LOAD are used - see exec.c). filesz bytes are read
// from the file at off; if memsz > filesz the remainder (typically
// .bss) is left zeroed, since allocuvm() zero-fills new pages.
struct proghdr {
  uint type;
  uint off;
  uint vaddr;
  uint paddr;
  uint filesz;
  uint memsz;
  uint flags;
  uint align;
};
#endif

// Values for Proghdr type
#define ELF_PROG_LOAD           1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4
