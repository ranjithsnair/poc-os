/*
 * Hand-written ring-3 test program exercising the TTY layer: registers
 * itself as the foreground process via SYS_IOCTL(TIOCSPGRP) (as a real
 * shell would before running a foreground command), switches to raw
 * mode via TCSETS and reads it back via TCGETS to confirm the round
 * trip, then installs a SIGINT handler and waits. kmain's idle loop
 * (see main.c) simulates a physically-typed Ctrl-C a few ticks after
 * boot by calling console_feed_char(0x03) directly -- the same call
 * keyboard.c's IRQ1 handler makes for a real Ctrl-C keypress, since
 * there's no way to inject a PS/2 scancode in this environment -- which
 * should land on exactly this process, being the registered foreground
 * pid. Expect "i" (ioctl round-trip ok), then "c" (the handler actually
 * ran) and "!" (the interrupted wait loop resumed correctly).
 */
.section .text

.global tty_test_program
.global tty_test_program_end

tty_test_program:
    mov $12, %rax              /* SYS_GETPID */
    int $0x80
    mov %rax, %rbx               /* own pid */

    mov $0, %rdi                    /* fd (ignored) */
    mov $4, %rsi                      /* TIOCSPGRP */
    mov %rbx, %rdx
    mov $11, %rax                       /* SYS_IOCTL */
    int $0x80

    /* Raw mode (ICANON clear, ECHO set), then read it back. */
    sub $16, %rsp
    movl $2, (%rsp)                       /* POC_ECHO only */
    mov $0, %rdi
    mov $2, %rsi                            /* TCSETS */
    mov %rsp, %rdx
    mov $11, %rax
    int $0x80

    mov $0, %rdi
    mov $1, %rsi                              /* TCGETS */
    mov %rsp, %rdx
    mov $11, %rax
    int $0x80
    movl (%rsp), %ecx
    add $16, %rsp
    cmp $2, %ecx
    jne ioctl_fail
    mov $0x69, %rdi                              /* 'i' */
    jmp ioctl_report
ioctl_fail:
    mov $0x78, %rdi                                /* 'x' */
ioctl_report:
    mov $0, %rax
    int $0x80

    lea handler(%rip), %rsi
    lea restorer(%rip), %rdx
    mov $2, %rdi                  /* SIGINT */
    mov $13, %rax                   /* SYS_SIGACTION */
    int $0x80

    mov $17, %rax                     /* SYS_CLOCK_GET -- baseline */
    int $0x80
    add $30, %rax                       /* wait up to ~30 ticks for the injected Ctrl-C */
    mov %rax, %r14
spin:
    mov $17, %rax
    int $0x80
    cmp %r14, %rax
    jl spin

    mov $0x21, %rdi                       /* '!' */
    mov $0, %rax
    int $0x80

    mov $1, %rax                             /* SYS_EXIT */
    mov $0, %rdi
    int $0x80

handler:
    mov $0x63, %rdi           /* 'c' */
    mov $0, %rax
    int $0x80
    ret

restorer:
    mov %rsp, %rdi
    mov $23, %rax               /* SYS_SIGRETURN */
    int $0x80
1:
    jmp 1b
tty_test_program_end:

.section .note.GNU-stack,"",@progbits
