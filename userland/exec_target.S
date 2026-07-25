/*
 * A tiny standalone ELF64 executable, installed onto disk.img as
 * /exectgt, used to verify SYS_EXECVE: prints "OK\n" via raw int 0x80
 * SYS_WRITE_CHAR, then exits with a distinctive status. Being a normal
 * linked ELF, it can use absolute addressing freely -- elf_load() maps
 * every PT_LOAD segment at the exact virtual address userland/linker.ld
 * linked it at, so there's no position-independence constraint here.
 */
.section .text
.global _start
_start:
    mov $0x4F, %rdi     /* 'O' */
    mov $0, %rax        /* SYS_WRITE_CHAR */
    int $0x80
    mov $0x4B, %rdi     /* 'K' */
    mov $0, %rax
    int $0x80
    mov $0x0A, %rdi     /* '\n' */
    mov $0, %rax
    int $0x80
    mov $1, %rax        /* SYS_EXIT */
    mov $42, %rdi       /* distinctive exit status */
    int $0x80
1:
    jmp 1b
