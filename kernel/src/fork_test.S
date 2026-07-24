/*
 * Hand-written ring-3 test program exercising SYS_FORK/SYS_WAITPID: forks
 * once, the child prints 'C' and exits with status 7, the parent polls
 * SYS_WAITPID (never blocks -- see syscall.c) until it reaps that child,
 * then prints 'P' followed by the reaped exit status as a decimal digit.
 * Same position-independence constraint as user_test.S (process_create()
 * copies this blob to a physical frame chosen at runtime): every
 * instruction here is a register/immediate op or a PC-relative jump, with
 * no absolute-address references at all.
 */
.section .text

.global fork_test_program
.global fork_test_program_end

fork_test_program:
    mov $18, %rax           /* SYS_FORK */
    int $0x80
    cmp $0, %rax
    je child

parent:
    sub $16, %rsp            /* room for the int* SYS_WAITPID writes through */
wait_loop:
    mov %rsp, %rsi
    mov $-1, %rdi            /* target pid: -1 = any child */
    mov $20, %rax            /* SYS_WAITPID */
    int $0x80
    cmp $0, %rax
    jle wait_loop            /* 0 = no zombie yet, keep polling */

    mov $0x50, %rdi           /* 'P' */
    mov $0, %rax
    int $0x80
    movl (%rsp), %ecx         /* reaped exit status (expect 7) */
    add $0x30, %rcx            /* -> ASCII digit, assuming a single-digit status */
    mov %rcx, %rdi
    mov $0, %rax
    int $0x80

    mov $1, %rax               /* SYS_EXIT */
    mov $0, %rdi
    int $0x80

child:
    mov $0x43, %rdi           /* 'C' */
    mov $0, %rax
    int $0x80
    mov $1, %rax              /* SYS_EXIT */
    mov $7, %rdi
    int $0x80

1:
    jmp 1b
fork_test_program_end:

.section .note.GNU-stack,"",@progbits
