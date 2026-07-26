/*
 * Raw `int $0x80` trap -- see kernel/src/syscall.h: rax = number,
 * rdi/rsi/rdx = up to three arguments, return value in rax. The kernel's
 * syscall_common_stub only ever overwrites rax (see asm_stubs.S), so no
 * other register needs clobbering here beyond the compiler-visible
 * "memory" barrier (a syscall can read/write arbitrary user memory
 * through pointer arguments).
 */
#include <bits/syscall.h>

extern "C" long __do_syscall_ret(unsigned long ret) {
	/* PoC-OS's raw ABI returns plain values (0/positive = success,
	 * negative = an already-meaningful error/status code), not Linux's
	 * -errno encoding -- nothing to translate here. Each sysdeps.cpp
	 * hook is what maps a particular negative return into the errno a
	 * POSIX caller expects, since only the hook itself knows what that
	 * specific syscall's negative values mean (see e.g. SYS_PIPE_AGAIN
	 * in kernel/src/syscall.h). */
	return (long)ret;
}

using sc_word_t = long;

sc_word_t __do_syscall0(long sc) {
	long ret;
	asm volatile ("int $0x80" : "=a"(ret) : "a"(sc) : "memory");
	return ret;
}

sc_word_t __do_syscall1(long sc, sc_word_t a1) {
	long ret;
	asm volatile ("int $0x80" : "=a"(ret) : "a"(sc), "D"(a1) : "memory");
	return ret;
}

sc_word_t __do_syscall2(long sc, sc_word_t a1, sc_word_t a2) {
	long ret;
	asm volatile ("int $0x80" : "=a"(ret) : "a"(sc), "D"(a1), "S"(a2) : "memory");
	return ret;
}

sc_word_t __do_syscall3(long sc, sc_word_t a1, sc_word_t a2, sc_word_t a3) {
	long ret;
	asm volatile ("int $0x80" : "=a"(ret) : "a"(sc), "D"(a1), "S"(a2), "d"(a3) : "memory");
	return ret;
}
