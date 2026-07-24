/*
 * Hand-written ring-3 test program exercising SYS_PIPE/SYS_DUP2 (dup2
 * only implicitly, in that fork()ing after pipe() -- exactly like a real
 * shell setting up a pipeline -- is the scenario the refcounting in
 * process_fd_dup2()/process_fork() has to get right): creates a pipe,
 * forks, the parent writes "hi\n" then closes its write end (so the
 * child sees true EOF, not just an empty buffer), and the child reads it
 * back one byte at a time via SYS_READ and echoes each byte via
 * SYS_WRITE_CHAR. Expect "hi" to appear in the serial log, followed by
 * both processes exiting cleanly. Same position-independence constraint
 * as the other flat-blob test programs.
 */
.section .text

.global pipe_test_program
.global pipe_test_program_end

pipe_test_program:
    sub $16, %rsp
    mov %rsp, %rdi
    mov $21, %rax             /* SYS_PIPE */
    int $0x80
    movl (%rsp), %r12d          /* read_fd -- r12/r13 survive fork (copied via *regs) */
    movl 4(%rsp), %r13d          /* write_fd */
    add $16, %rsp

    mov $18, %rax                 /* SYS_FORK */
    int $0x80
    cmp $0, %rax
    je child

parent:
    mov %r12, %rdi                  /* close(read_fd) -- parent only writes */
    mov $5, %rax                     /* SYS_CLOSE */
    int $0x80

    sub $16, %rsp
    movb $0x68, (%rsp)                 /* 'h' */
    movb $0x69, 1(%rsp)                 /* 'i' */
    movb $0x0A, 2(%rsp)                  /* '\n' */
    mov %r13, %rdi
    mov %rsp, %rsi
    mov $3, %rdx
    mov $3, %rax                          /* SYS_WRITE */
    int $0x80
    add $16, %rsp

    mov %r13, %rdi
    mov $5, %rax                           /* SYS_CLOSE(write_fd) -- lets the child see EOF */
    int $0x80

wait_loop:
    mov $-1, %rdi
    mov $0, %rsi
    mov $20, %rax                            /* SYS_WAITPID */
    int $0x80
    cmp $0, %rax
    jle wait_loop

    mov $1, %rax                               /* SYS_EXIT */
    mov $0, %rdi
    int $0x80

child:
    mov %r13, %rdi                              /* close(write_fd) -- child only reads */
    mov $5, %rax
    int $0x80

    sub $16, %rsp
read_loop:
    mov %r12, %rdi
    mov %rsp, %rsi
    mov $1, %rdx
    mov $2, %rax                                  /* SYS_READ, one byte at a time */
    int $0x80
    cmp $0, %rax
    je read_done                                    /* 0 = true EOF (all writers closed) */
    jl read_loop                                     /* SYS_PIPE_AGAIN (-2) = poll again */

    movzbq (%rsp), %rdi
    mov $0, %rax                                       /* SYS_WRITE_CHAR */
    int $0x80
    jmp read_loop

read_done:
    add $16, %rsp
    mov $1, %rax                                         /* SYS_EXIT */
    mov $0, %rdi
    int $0x80

1:
    jmp 1b
pipe_test_program_end:

.section .note.GNU-stack,"",@progbits
