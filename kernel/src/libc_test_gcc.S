/*
 * Phase 2 verification: identical to libc_test.S except it execve()s
 * "hellogcc" -- the same hello_libc.c source, but compiled and linked
 * with the real cross x86_64-elf-gcc (toolchain/cross/) against the
 * Phase-1 mlibc sysroot, instead of clang/ld.lld. If this prints the
 * same "hello from libc" as libc_test.S's clang-built "hellolib", the
 * cross GCC's --with-sysroot wiring, its own crt1.o/crtbegin.o/crtend.o
 * handshake with userland/linker.ld's .init_array/.fini_array sections,
 * and libgcc all work end-to-end. Only reached if SYS_EXECVE fails:
 * prints 'E' and exits.
 */
.section .text

.global libc_test_gcc_program
.global libc_test_gcc_program_end

libc_test_gcc_program:
    lea path(%rip), %rax

    push $0                  /* envp[0] = NULL */
    mov %rsp, %rdx             /* rdx = envp array pointer */

    push $0                     /* argv[1] = NULL */
    push %rax                    /* argv[0] = &path */
    mov %rsp, %rsi                 /* rsi = argv array pointer */

    mov %rax, %rdi                  /* rdi = path pointer (before rax is clobbered below) */
    mov $19, %rax             /* SYS_EXECVE */
    int $0x80

    /* Only reached if SYS_EXECVE failed. */
    mov $0x45, %rdi             /* 'E' */
    mov $0, %rax
    int $0x80
    mov $1, %rax
    mov $98, %rdi
    int $0x80
1:
    jmp 1b

.align 8
path:
    .asciz "hellogcc"
libc_test_gcc_program_end:

.section .note.GNU-stack,"",@progbits
