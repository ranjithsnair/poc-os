// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian

// File header
// entry is where execution starts (used by both boot/bootmain.c, to
// jump into the kernel, and exec(), to set the new process's eip);
// phoff/phnum locate the array of proghdr entries below, which is what
// both of those actually load into memory.
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

// Values for Proghdr type
#define ELF_PROG_LOAD           1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4
