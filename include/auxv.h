// ELF auxiliary vector tags, for the AT_* pairs kernel/exec.c's execve()
// places after argv[]/envp[] on a new process's initial stack. Not a
// poc-os invention: these numeric values are the same ones musl (and
// Linux) use - musl/include/elf.h has the full list - so only the
// handful __init_libc()/__init_tls() actually read are reproduced here.
#define AT_NULL     0  // terminates the auxv array
#define AT_PHDR     3  // address of the program headers
#define AT_PHENT    4  // size of one program header entry
#define AT_PHNUM    5  // number of program header entries
#define AT_PAGESZ   6  // system page size
#define AT_BASE     7  // interpreter base address (0: no interpreter)
#define AT_ENTRY    9  // program's ELF entry point
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_HWCAP   16  // CPU feature bitmask (0: none reported)
#define AT_SECURE  23
#define AT_RANDOM  25  // address of 16 random-ish bytes
#define AT_EXECFN  31  // address of the program's path string
