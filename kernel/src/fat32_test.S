/*
 * Hand-written ring-3 test program exercising the writable FAT32 disk
 * end-to-end: creates "/testfil.txt" (SYS_OPEN with O_CREAT|O_WRONLY),
 * writes "HELLO", closes it, reopens read-only, reads it back and
 * compares -- expect 'W' (or 'w' if anything didn't match/succeed).
 * Then SYS_MKDIR("/testdir") and repeats the same create/write/reopen/
 * read/compare dance at "/testdir/nested.txt", proving subdirectory
 * traversal works too -- expect 'D' (or 'd').
 *
 * Same position-independence constraint as the other flat-blob test
 * programs: path/data strings are read-only (safe to reference via
 * `lea ...(%rip)`, since nothing here ever writes to its own code
 * page -- see exec_test.S's doc comment for why that specifically would
 * be unsafe), and the read-back buffers live on the stack instead.
 */
.section .text

.global fat32_test_program
.global fat32_test_program_end

fat32_test_program:
    lea path1(%rip), %rdi
    mov $0x41, %rsi           /* O_CREAT(0x40) | O_WRONLY(0x1) */
    mov $4, %rax                /* SYS_OPEN */
    int $0x80
    cmp $0, %rax
    jl fail_w
    mov %rax, %r12

    mov %r12, %rdi
    lea data1(%rip), %rsi
    mov $5, %rdx
    mov $3, %rax                  /* SYS_WRITE */
    int $0x80
    cmp $5, %rax
    jne fail_w

    mov %r12, %rdi
    mov $5, %rax                    /* SYS_CLOSE */
    int $0x80

    lea path1(%rip), %rdi
    mov $0, %rsi                      /* O_RDONLY */
    mov $4, %rax
    int $0x80
    cmp $0, %rax
    jl fail_w
    mov %rax, %r12

    sub $16, %rsp
    mov %r12, %rdi
    mov %rsp, %rsi
    mov $5, %rdx
    mov $2, %rax                        /* SYS_READ */
    int $0x80
    cmp $5, %rax
    jne fail_w_cleanup

    lea data1(%rip), %rbx
    xor %rcx, %rcx
cmp_loop1:
    movb (%rsp,%rcx), %al
    cmp (%rbx,%rcx), %al
    jne fail_w_cleanup
    inc %rcx
    cmp $5, %rcx
    jl cmp_loop1

    add $16, %rsp
    mov %r12, %rdi
    mov $5, %rax                          /* SYS_CLOSE */
    int $0x80
    mov $0x57, %rdi                         /* 'W' */
    jmp report_w
fail_w_cleanup:
    add $16, %rsp
fail_w:
    mov $0x77, %rdi                           /* 'w' */
report_w:
    mov $0, %rax
    int $0x80

    lea dirpath(%rip), %rdi
    mov $25, %rax                /* SYS_MKDIR */
    int $0x80
    cmp $0, %rax
    jne fail_d

    lea path2(%rip), %rdi
    mov $0x41, %rsi
    mov $4, %rax                   /* SYS_OPEN */
    int $0x80
    cmp $0, %rax
    jl fail_d
    mov %rax, %r12

    mov %r12, %rdi
    lea data2(%rip), %rsi
    mov $3, %rdx
    mov $3, %rax                     /* SYS_WRITE */
    int $0x80
    cmp $3, %rax
    jne fail_d

    mov %r12, %rdi
    mov $5, %rax                       /* SYS_CLOSE */
    int $0x80

    lea path2(%rip), %rdi
    mov $0, %rsi
    mov $4, %rax
    int $0x80
    cmp $0, %rax
    jl fail_d
    mov %rax, %r12

    sub $16, %rsp
    mov %r12, %rdi
    mov %rsp, %rsi
    mov $3, %rdx
    mov $2, %rax                         /* SYS_READ */
    int $0x80
    cmp $3, %rax
    jne fail_d_cleanup

    lea data2(%rip), %rbx
    xor %rcx, %rcx
cmp_loop2:
    movb (%rsp,%rcx), %al
    cmp (%rbx,%rcx), %al
    jne fail_d_cleanup
    inc %rcx
    cmp $3, %rcx
    jl cmp_loop2

    add $16, %rsp
    mov %r12, %rdi
    mov $5, %rax                           /* SYS_CLOSE */
    int $0x80
    mov $0x44, %rdi                          /* 'D' */
    jmp report_d
fail_d_cleanup:
    add $16, %rsp
fail_d:
    mov $0x64, %rdi                            /* 'd' */
report_d:
    mov $0, %rax
    int $0x80

    mov $1, %rax                                 /* SYS_EXIT */
    mov $0, %rdi
    int $0x80
1:
    jmp 1b

.align 8
path1:
    .asciz "/testfil.txt"
data1:
    .ascii "HELLO"
dirpath:
    .asciz "/testdir"
path2:
    .asciz "/testdir/nested.txt"
data2:
    .ascii "SUB"
fat32_test_program_end:

.section .note.GNU-stack,"",@progbits
