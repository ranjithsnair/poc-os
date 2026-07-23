/*
 * See elf.h. The ELF64 structures below are hand-declared (matching the
 * System V ABI spec byte-for-byte) rather than pulled from a system
 * <elf.h> -- this is a freestanding, no-libc build, same reasoning as
 * limine.h being vendored instead of assumed to exist.
 */
#include <stdint.h>
#include <stddef.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "serial.h"

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define ET_EXEC      2
#define EM_X86_64    62
#define PT_LOAD      1
#define PF_X (1u << 0)
#define PF_W (1u << 1)

#define PAGE_MASK (~(uint64_t)(PMM_FRAME_SIZE - 1))

static int valid_header(const Elf64_Ehdr *eh, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) {
        return 0;
    }
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        serial_print("PoC-OS: elf_load: not an ELF file.\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        serial_print("PoC-OS: elf_load: not a 64-bit little-endian ELF.\n");
        return 0;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_print("PoC-OS: elf_load: wrong machine type (not x86-64).\n");
        return 0;
    }
    if (eh->e_type != ET_EXEC) {
        serial_print("PoC-OS: elf_load: not a static ET_EXEC (PIE/ET_DYN unsupported -- no dynamic loader).\n");
        return 0;
    }
    return 1;
}

/* Maps [vaddr, vaddr+memsz) page by page, copying filesz bytes of file
 * content (starting at file_data+file_off) into the start of that
 * range and leaving the rest zeroed (pmm_alloc_frame() already zeroes
 * every frame it hands out, so BSS needs no explicit zero-fill here). */
static int map_segment(uint64_t pml4_phys, uint64_t vaddr, uint64_t memsz,
                        const uint8_t *file_data, uint64_t filesz, uint32_t p_flags) {
    uint64_t flags = VMM_USER;
    if (p_flags & PF_W) {
        flags |= VMM_WRITABLE;
    }
    if (!(p_flags & PF_X)) {
        flags |= VMM_NX;
    }

    uint64_t seg_start = vaddr & PAGE_MASK;
    uint64_t seg_end = (vaddr + memsz + PMM_FRAME_SIZE - 1) & PAGE_MASK;

    for (uint64_t page_va = seg_start; page_va < seg_end; page_va += PMM_FRAME_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            serial_print("PoC-OS: elf_load: out of memory mapping a PT_LOAD segment.\n");
            return 0;
        }
        uint8_t *kpage = (uint8_t *)vmm_phys_to_virt(frame);

        /* Copy whatever part of [vaddr, vaddr+filesz) overlaps this page. */
        uint64_t file_start = vaddr;
        uint64_t file_end = vaddr + filesz;
        uint64_t page_end = page_va + PMM_FRAME_SIZE;
        uint64_t copy_start = (file_start > page_va) ? file_start : page_va;
        uint64_t copy_end = (file_end < page_end) ? file_end : page_end;
        if (copy_start < copy_end) {
            uint64_t src_off = copy_start - vaddr;
            uint64_t dst_off = copy_start - page_va;
            memcpy(kpage + dst_off, file_data + src_off, copy_end - copy_start);
        }

        vmm_map(pml4_phys, page_va, frame, flags);
    }
    return 1;
}

int elf_load(uint64_t pml4_phys, const uint8_t *data, uint64_t size, struct elf_load_result *out) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (!valid_header(eh, size)) {
        return 0;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        serial_print("PoC-OS: elf_load: program header table out of bounds.\n");
        return 0;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size) {
            serial_print("PoC-OS: elf_load: PT_LOAD segment data out of bounds.\n");
            return 0;
        }
        if (!map_segment(pml4_phys, ph->p_vaddr, ph->p_memsz, data + ph->p_offset, ph->p_filesz, ph->p_flags)) {
            return 0;
        }
    }

    uint64_t stack_bottom = VMM_USER_STACK_TOP - (uint64_t)VMM_USER_STACK_PAGES * PMM_FRAME_SIZE;
    for (uint64_t page_va = stack_bottom; page_va < VMM_USER_STACK_TOP; page_va += PMM_FRAME_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            serial_print("PoC-OS: elf_load: out of memory mapping the user stack.\n");
            return 0;
        }
        vmm_map(pml4_phys, page_va, frame, VMM_USER | VMM_WRITABLE | VMM_NX);
    }

    out->entry = eh->e_entry;
    out->stack_top = VMM_USER_STACK_TOP;
    return 1;
}

/* Writes a single byte/u64 into an already-mapped page of `pml4_phys`
 * via vmm_translate()+vmm_phys_to_virt() -- this address space isn't
 * necessarily the one currently loaded into CR3 (process creation
 * happens before the new process is ever scheduled), so the HHDM is the
 * only way to reach its pages. */
static int write_user_byte(uint64_t pml4_phys, uint64_t va, uint8_t val) {
    uint64_t phys = vmm_translate(pml4_phys, va);
    if (phys == UINT64_MAX) {
        return 0;
    }
    uint8_t *kptr = (uint8_t *)vmm_phys_to_virt(phys);
    *kptr = val;
    return 1;
}

static int write_user_u64(uint64_t pml4_phys, uint64_t va, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        if (!write_user_byte(pml4_phys, va + i, (uint8_t)(val >> (8 * i)))) {
            return 0;
        }
    }
    return 1;
}

#define ELF_STACK_MAX_ARGS 16

static uint64_t elf_strlen(const char *s) {
    uint64_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

uint64_t elf_build_user_stack(uint64_t pml4_phys, uint64_t stack_top,
                               int argc, const char *const argv[],
                               int envc, const char *const envp[]) {
    if (argc < 0 || argc > ELF_STACK_MAX_ARGS || envc < 0 || envc > ELF_STACK_MAX_ARGS) {
        return 0;
    }

    uint64_t cursor = stack_top;
    uint64_t argv_va[ELF_STACK_MAX_ARGS];
    uint64_t envp_va[ELF_STACK_MAX_ARGS];

    /* Strings first, growing downward -- order among themselves doesn't
     * matter, only that each ends up NUL-terminated at some VA we can
     * point argv[i]/envp[i] at. */
    for (int i = 0; i < argc; i++) {
        uint64_t len = elf_strlen(argv[i]) + 1;
        cursor -= len;
        for (uint64_t j = 0; j < len; j++) {
            if (!write_user_byte(pml4_phys, cursor + j, (uint8_t)argv[i][j])) {
                return 0;
            }
        }
        argv_va[i] = cursor;
    }
    for (int i = 0; i < envc; i++) {
        uint64_t len = elf_strlen(envp[i]) + 1;
        cursor -= len;
        for (uint64_t j = 0; j < len; j++) {
            if (!write_user_byte(pml4_phys, cursor + j, (uint8_t)envp[i][j])) {
                return 0;
            }
        }
        envp_va[i] = cursor;
    }

    cursor &= ~(uint64_t)0xF; /* align before the pointer arrays */

    /* [argc][argv[0..argc-1]][NULL][envp[0..envc-1]][NULL][AT_NULL,0] */
    uint64_t array_words = 1 + (uint64_t)(argc + 1) + (uint64_t)(envc + 1) + 2;
    cursor -= array_words * 8;
    cursor &= ~(uint64_t)0xF;

    uint64_t p = cursor;
    if (!write_user_u64(pml4_phys, p, (uint64_t)argc)) return 0;
    p += 8;
    for (int i = 0; i < argc; i++) {
        if (!write_user_u64(pml4_phys, p, argv_va[i])) return 0;
        p += 8;
    }
    if (!write_user_u64(pml4_phys, p, 0)) return 0;
    p += 8;
    for (int i = 0; i < envc; i++) {
        if (!write_user_u64(pml4_phys, p, envp_va[i])) return 0;
        p += 8;
    }
    if (!write_user_u64(pml4_phys, p, 0)) return 0;
    p += 8;
    /* auxv: AT_NULL (0) terminator pair only -- crt0 stops there. */
    if (!write_user_u64(pml4_phys, p, 0)) return 0;
    p += 8;
    if (!write_user_u64(pml4_phys, p, 0)) return 0;

    return cursor;
}
