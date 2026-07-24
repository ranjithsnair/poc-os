/*
 * Minimal static (ET_EXEC, non-PIE) ELF64/x86-64 loader. There's no
 * dynamic linker in this kernel and none is planned soon, so PT_INTERP/
 * ET_DYN/relocations are deliberately out of scope -- elf_load() rejects
 * anything that isn't a plain static executable rather than silently
 * mis-handling it.
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Where execution should start, the top of the stack region elf_load()
 * has already mapped (but not yet populated with argc/argv/envp -- see
 * elf_build_user_stack()), and the program header table's *virtual*
 * address/shape -- needed only for the AT_PHDR/AT_PHENT/AT_PHNUM auxv
 * entries elf_build_user_stack() writes, which mlibc's static-binary
 * startup path (options/rtld/generic/main.cpp's interpreterMain(),
 * unconditionally invoked even for non-PIE binaries -- see
 * MLIBC_STATIC_BUILD) reads during its own TLS/segment setup and
 * dereferences unconditionally, auxv or not. */
struct elf_load_result {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t phdr_vaddr;  /* 0 if e_phoff didn't fall inside any PT_LOAD segment */
    uint64_t phentsize;
    uint64_t phnum;
};

/* Parses `data[0..size)` as an ELF64 image, validates it's a static
 * x86-64 executable (ELFCLASS64, little-endian, EM_X86_64, ET_EXEC --
 * ET_DYN is rejected outright), maps every PT_LOAD segment into the
 * given address space via vmm_map() (honoring p_flags, zero-filling
 * p_memsz - p_filesz), and maps a fixed-size stack region below
 * VMM_USER_STACK_TOP. Returns 1 on success (filling *out), 0 on any
 * validation or out-of-memory failure. */
int elf_load(uint64_t pml4_phys, const uint8_t *data, uint64_t size, struct elf_load_result *out);

/* Writes argc, argv[], envp[], and an auxv (AT_PHDR/AT_PHENT/AT_PHNUM/
 * AT_BASE/AT_PAGESZ/AT_ENTRY, AT_NULL-terminated -- see
 * struct elf_load_result's doc comment for why these specific entries
 * matter) onto the top of the stack region elf_load() already mapped,
 * following the System V x86-64 initial-stack layout crt0 expects.
 * `argc`/`envc` must not exceed 15 (kept small deliberately -- this is
 * boot-time process creation, not a general exec() path yet). Returns
 * the resulting %rsp value the process should start with, or 0 on
 * failure (arguments too large for the reserved stack region). */
uint64_t elf_build_user_stack(uint64_t pml4_phys, const struct elf_load_result *elf,
                               int argc, const char *const argv[],
                               int envc, const char *const envp[]);

#endif
