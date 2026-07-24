/*
 * SYS_SIGACTION's `restorer` argument (kernel/src/syscall.h) must be an
 * *executable* address whose only job is to invoke SYS_SIGRETURN with
 * %rsp pointing at the saved `struct registers` frame the kernel pushed
 * before entering the handler -- see kernel/src/signal_test.S's
 * `restorer:` label (this is a straight copy of it, just exported under
 * a name sysdeps.cpp can reference from C++). A real handler's `ret`
 * pops exactly this address as its return address, so %rsp already
 * points at the saved frame the instant we land here.
 */
.section .text

.global __pocos_sigreturn_trampoline

__pocos_sigreturn_trampoline:
    mov %rsp, %rdi
    mov $23, %rax   /* SYS_SIGRETURN */
    int $0x80
1:
    jmp 1b

.section .note.GNU-stack,"",@progbits
