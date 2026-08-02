/* Copyright 2011-2012 Nicholas J. Kain, licensed under standard MIT license */
/* Forked for poc-os: the original hardcodes the `syscall` instruction and
 * Linux's arch_prctl number (158) directly, bypassing
 * musl/arch/x86_64/syscall_arch.h entirely - it has to be forked
 * separately, the same way that header was, rather than just picking up
 * poc-os's convention automatically. Uses poc-os's own SYS_arch_prctl
 * (23, see include/syscall.h) and `int $0x40` (T_SYSCALL) instead. */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rsi           /* shift for syscall: addr becomes arg2 */
	movl $0x1002,%edi       /* ARCH_SET_FS */
	movl $23,%eax           /* poc-os's SYS_arch_prctl, not Linux's 158 */
	int $0x40               /* poc-os syscall convention, not `syscall` */
	ret
