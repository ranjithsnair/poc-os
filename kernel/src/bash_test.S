/*
 * Phase 3 verification: execve()s into the cross-compiled GNU bash
 * binary (installed onto the FAT32 disk as "bash" -- see the top-level
 * Makefile's disk.img rule and toolchain/build-bash/bash's build recipe)
 * in isolation, before it's wired up as the kernel's init process
 * (Phase 4). argv={"bash"}, envp={"PS1=$ ", "PATH=/", "HOME=/",
 * "TERM=dumb", NULL} -- enough for bash's own startup path (prompt
 * string decoding, PATH search, tilde expansion) to have something other
 * than NULL to work with. TERM specifically is not optional: shell.c's
 * interactive-shell startup calls get_string_value("TERM") and does
 * STREQ(term, "emacs") on the result with no NULL check -- found by
 * omitting it and hitting exactly that NULL deref. Nothing here checks
 * bash's actual output; this just needs bash to start running without
 * immediately faulting, per the plan's Phase 3 verification step --
 * confirmed instead by watching the serial log for a fault/reboot vs.
 * bash actually printing its own prompt.
 */
.section .text

.global bash_test_program
.global bash_test_program_end

bash_test_program:
    lea path(%rip), %rax

    push $0                  /* envp[4] = NULL */
    lea env_term(%rip), %rbx
    push %rbx                /* envp[3] = "TERM=dumb" */
    lea env_home(%rip), %rbx
    push %rbx                /* envp[2] = "HOME=/" */
    lea env_path(%rip), %rbx
    push %rbx                /* envp[1] = "PATH=/" */
    lea env_ps1(%rip), %rbx
    push %rbx                /* envp[0] = "PS1=$ " */
    mov %rsp, %rdx            /* rdx = envp array pointer */

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
    .asciz "bash"
env_ps1:
    .asciz "PS1=$ "
env_path:
    .asciz "PATH=/"
env_home:
    .asciz "HOME=/"
env_term:
    .asciz "TERM=dumb"
bash_test_program_end:

.section .note.GNU-stack,"",@progbits
