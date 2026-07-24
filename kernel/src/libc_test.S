/*
 * Hand-written ring-3 test program exercising the Phase 1 mlibc port:
 * execve()s into userland/hello_libc.elf (installed onto the FAT32 disk
 * as "hellolib"), a completely ordinary C program --
 * `printf("hello from libc\n")` -- linked against mlibc's static libc.a
 * and PoC-OS's own sysdeps (toolchain/mlibc-sysdeps-pocos/). If this
 * prints correctly, argv/envp setup, crt1.S's entry sequence, TLS
 * (TcbSet), and mlibc's own stdio buffering all worked end-to-end on top
 * of the raw kernel syscalls the earlier tests exercised individually.
 * Only reached if SYS_EXECVE fails: prints 'E' and exits. Same shape as
 * exec_test.S -- see its doc comment for why argv/envp are built on the
 * stack rather than as static data alongside the code.
 */
.section .text

.global libc_test_program
.global libc_test_program_end

libc_test_program:
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
    .asciz "hellolib"
libc_test_program_end:

.section .note.GNU-stack,"",@progbits
