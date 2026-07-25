/*
 * ELF64/x86-64 loader. Handles both plain static ET_EXEC executables and
 * ET_DYN images (PIE executables and the PT_INTERP dynamic linker itself)
 * -- see elf_load()'s `load_base` parameter and elf_find_interp(). There's
 * still no in-kernel relocation processing or dynamic-symbol resolution:
 * for an ET_DYN main executable that names a PT_INTERP, the kernel's job
 * is only to map both images (see process.c's orchestration of the two)
 * and hand off to the interpreter, which does all of that itself in
 * userspace (mlibc/options/rtld).
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Where execution should start (this image's own entry point, base-
 * adjusted for ET_DYN), the top of the stack region (filled in by
 * elf_map_user_stack(), not elf_load() itself -- see its doc comment),
 * and the program header table's *virtual* address/shape -- needed only
 * for the AT_PHDR/AT_PHENT/AT_PHNUM auxv entries elf_build_user_stack()
 * writes, which mlibc's rtld startup path (options/rtld/generic/
 * main.cpp's interpreterMain(), unconditionally invoked even for non-PIE
 * static binaries -- see MLIBC_STATIC_BUILD) reads during its own TLS/
 * segment setup and dereferences unconditionally, auxv or not. */
struct elf_load_result {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t phdr_vaddr;  /* 0 if e_phoff didn't fall inside any PT_LOAD segment */
    uint64_t phentsize;
    uint64_t phnum;
};

/* Parses `data[0..size)` as an ELF64 image, validates it's an x86-64
 * executable (ELFCLASS64, little-endian, EM_X86_64, ET_EXEC or ET_DYN),
 * and maps every PT_LOAD segment into the given address space via
 * vmm_map() (honoring p_flags, zero-filling p_memsz - p_filesz), adding
 * `load_base` to every segment's p_vaddr first. Pass `load_base` 0 for a
 * plain ET_EXEC (its p_vaddrs are already absolute); a nonzero, page-
 * aligned constant for an ET_DYN image (a PIE main executable or the
 * interpreter itself), chosen by the caller so multiple images loaded
 * into the same address space (main executable + interpreter) don't
 * overlap -- see process.c's ELF_PIE_BASE/ELF_INTERP_BASE. Does not map
 * a stack (see elf_map_user_stack()) or resolve PT_INTERP (see
 * elf_find_interp()) -- both are the caller's job, since a PT_INTERP
 * binary needs two elf_load() calls into one address space sharing a
 * single stack. Returns 1 on success (filling *out; out->stack_top is
 * left unset), 0 on any validation or out-of-memory failure. */
int elf_load(uint64_t pml4_phys, const uint8_t *data, uint64_t size, uint64_t load_base,
             struct elf_load_result *out);

/* Returns 1 if `data[0..size)` starts with a valid-looking ELF64 x86-64
 * ET_DYN header (a PIE executable or the dynamic linker itself), 0 for
 * anything else (ET_EXEC, or too short to even hold a header) -- meant
 * to be checked before elf_load() to decide `load_base`: nonzero only
 * for ET_DYN, since an ET_EXEC's p_vaddrs are already absolute. Doesn't
 * duplicate elf_load()'s full validation (class/endianness/machine) --
 * a bogus image that passes this still gets rejected by elf_load() itself. */
int elf_is_dyn(const uint8_t *data, uint64_t size);

/* Scans `data[0..size)`'s program headers for a PT_INTERP segment and, if
 * one exists, copies its NUL-terminated interpreter path string into
 * `out` (bounded to `out_cap`, always NUL-terminated on success). Meant
 * to be called before elf_load() on the same main-executable image, so
 * the caller knows whether a second elf_load() (for the interpreter) is
 * needed at all. Returns 1 if a PT_INTERP segment was found and its path
 * fit in `out`, 0 otherwise (no PT_INTERP present, or the path didn't
 * fit -- both leave `out` untouched). */
int elf_find_interp(const uint8_t *data, uint64_t size, char *out, uint64_t out_cap);

/* Maps the fixed-size user stack region below VMM_USER_STACK_TOP -- split
 * out of elf_load() so it's called exactly once per *process* (not once
 * per image), even when a PT_INTERP binary means two elf_load() calls
 * share one address space. Returns 1 on success, 0 on out-of-memory. */
int elf_map_user_stack(uint64_t pml4_phys);

/* Writes argc, argv[], envp[], and an auxv (AT_PHDR/AT_PHENT/AT_PHNUM/
 * AT_BASE/AT_PAGESZ/AT_ENTRY, AT_NULL-terminated -- see
 * struct elf_load_result's doc comment for why these specific entries
 * matter) onto the top of the stack region elf_map_user_stack() already
 * mapped, following the System V x86-64 initial-stack layout crt0
 * expects. `elf` describes the *main* executable image (its ->entry
 * becomes AT_ENTRY, ->phdr_vaddr/phentsize/phnum become AT_PHDR/PHENT/
 * PHNUM, and ->stack_top must already be set by the caller -- see above)
 * regardless of whether a separate interpreter is also involved; `at_base`
 * is the interpreter's load base for AT_BASE, or 0 if the binary has no
 * PT_INTERP (statically linked). `argc`/`envc` must not exceed 15 (kept
 * small deliberately -- this is boot-time process creation, not a
 * general exec() path yet). Returns the resulting %rsp value the process
 * should start with, or 0 on failure (arguments too large for the
 * reserved stack region). */
uint64_t elf_build_user_stack(uint64_t pml4_phys, const struct elf_load_result *elf, uint64_t at_base,
                               int argc, const char *const argv[],
                               int envc, const char *const envp[]);

#endif
