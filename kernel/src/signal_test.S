/*
 * Hand-written ring-3 test program exercising SYS_SIGACTION/SYS_KILL/
 * SYS_SIGRETURN (a caught signal) and default-action termination:
 *
 * Subtest 1: installs a handler for SIGINT, sends itself SIGINT via
 * SYS_KILL, then spins on SYS_CLOCK_GET until a scheduler tick redirects
 * it into the handler (signal delivery only happens at a scheduling
 * opportunity -- see deliver_pending_signals() in process.c, not
 * synchronously inside SYS_KILL). The handler prints 'H' and returns
 * through `restorer` into SYS_SIGRETURN, which resumes the spin loop
 * exactly where it was interrupted; the loop then runs to completion
 * normally and prints 'S', proving both that the handler ran and that
 * the original context came back intact.
 *
 * Subtest 2: forks a child that just spins forever, sends it SIGTERM
 * (no handler installed, so the kernel's default action applies), and
 * waitpid()s for it -- expecting the POSIX "128+signum" exit status
 * (143), printing 'T' if so or 'X' if not.
 *
 * Same position-independence constraint as the other flat-blob test
 * programs (register/immediate ops and PC-relative references only).
 */
.section .text

.global signal_test_program
.global signal_test_program_end

signal_test_program:
    lea handler(%rip), %rsi
    lea restorer(%rip), %rdx
    mov $2, %rdi              /* SIGINT */
    mov $13, %rax               /* SYS_SIGACTION */
    int $0x80

    mov $12, %rax                 /* SYS_GETPID */
    int $0x80
    mov %rax, %rbx

    mov %rbx, %rdi
    mov $2, %rsi                    /* SIGINT */
    mov $24, %rax                     /* SYS_KILL */
    int $0x80

    mov $17, %rax                       /* SYS_CLOCK_GET -- baseline tick */
    int $0x80
    mov %rax, %r14
spin1:
    mov $17, %rax
    int $0x80
    cmp %r14, %rax
    je spin1                              /* redirected into `handler` before this ever loops long */

    mov $0x53, %rdi                         /* 'S' */
    mov $0, %rax
    int $0x80

    mov $18, %rax                             /* SYS_FORK */
    int $0x80
    cmp $0, %rax
    je child2
    mov %rax, %r15                              /* child's pid */

    mov %r15, %rdi
    mov $15, %rsi                                 /* SIGTERM */
    mov $24, %rax                                   /* SYS_KILL */
    int $0x80

    sub $16, %rsp
wait2:
    mov %rsp, %rsi
    mov $-1, %rdi
    mov $20, %rax                                     /* SYS_WAITPID */
    int $0x80
    cmp $0, %rax
    jle wait2

    movl (%rsp), %ecx
    cmp $143, %ecx                                      /* 128 + SIGTERM(15) */
    jne report_fail
    mov $0x54, %rdi                                       /* 'T' */
    jmp report_write
report_fail:
    mov $0x58, %rdi                                         /* 'X' */
report_write:
    mov $0, %rax
    int $0x80

    mov $1, %rax                                              /* SYS_EXIT */
    mov $0, %rdi
    int $0x80

child2:
    /* SIGTERM's default action terminates this with no cooperation
     * needed from the code running here -- just spin. */
spin2:
    mov $17, %rax
    int $0x80
    jmp spin2

handler:
    mov $0x48, %rdi           /* 'H' */
    mov $0, %rax
    int $0x80
    ret                         /* pops `restorer`'s address -- see SYS_SIGACTION's doc comment */

restorer:
    mov %rsp, %rdi
    mov $23, %rax               /* SYS_SIGRETURN */
    int $0x80
1:
    jmp 1b
signal_test_program_end:

.section .note.GNU-stack,"",@progbits
