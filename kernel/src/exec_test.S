/*
 * Hand-written ring-3 test program exercising SYS_EXECVE: replaces itself
 * with userland/exec_target.elf (installed onto the FAT32 disk as
 * "exectgt" -- 8.3-compatible, since fat32.c has no long-filename
 * support), which prints "OK\n" and exits with status 42 -- proof that
 * execve() tore down this blob's own address space and started running
 * a completely different, independently linked ELF image in its place.
 * Only reached if SYS_EXECVE fails: prints 'E' and exits.
 *
 * Same position-independence constraint as user_test.S/fork_test.S, with
 * one wrinkle: argv/envp are arrays of pointers that have to be built at
 * runtime, and process_create() maps this blob's *code* page read+exec
 * only (no VMM_WRITABLE) -- see process_create()'s vmm_map() call -- so
 * they can't live as writable data alongside the code the way a normal
 * linked ELF could. The stack page process_create() maps right after it
 * *is* writable, so argv/envp are built there instead, via plain `push`.
 */
.section .text

.global exec_test_program
.global exec_test_program_end

exec_test_program:
    lea path(%rip), %rax    /* rax = &path (a read-only reference is fine) */

    push $0                  /* envp[0] = NULL */
    mov %rsp, %rdx             /* rdx = envp array pointer */

    push $0                     /* argv[1] = NULL */
    push %rax                    /* argv[0] = &path */
    mov %rsp, %rsi                 /* rsi = argv array pointer */

    mov %rax, %rdi                  /* rdi = path pointer (before rax is clobbered below) */
    mov $19, %rax                    /* SYS_EXECVE */
    int $0x80

    /* Only reached if SYS_EXECVE failed. */
    mov $0x45, %rdi           /* 'E' */
    mov $0, %rax
    int $0x80
    mov $1, %rax
    mov $99, %rdi
    int $0x80
1:
    jmp 1b

.align 8
path:
    .asciz "exectgt"
exec_test_program_end:

.section .note.GNU-stack,"",@progbits
