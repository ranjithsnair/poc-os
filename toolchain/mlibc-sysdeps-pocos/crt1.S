/*
 * Entry point for every static PoC-OS executable. elf_build_user_stack()
 * (kernel/src/elf.c) lays out the initial stack exactly as the System V
 * x86-64 ABI expects (argc, argv[], NULL, envp[], NULL, auxv, NULL) --
 * the same layout crt1.S on any x86-64 Linux-like target relies on, so
 * this needs nothing PoC-OS-specific beyond calling into __mlibc_entry
 * with that raw stack pointer.
 */
.section .text
.global _start
_start:
	xor %rbp, %rbp        /* zero frame pointer marks the outermost frame for backtraces */
	mov %rsp, %rdi          /* first arg to __mlibc_entry: the raw entry stack pointer */
	lea main(%rip), %rsi      /* second arg: the real main() -- resolved at link time */
	call __mlibc_entry
	/* __mlibc_entry calls exit(), which never returns (Sysdeps<Exit> is
	 * a SYS_EXIT syscall the kernel never returns from either) -- this
	 * is unreachable, but a static binary must not fall off the end of
	 * .text into whatever comes next. */
1:
	jmp 1b

.section .note.GNU-stack,"",@progbits
