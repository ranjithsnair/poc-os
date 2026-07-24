/*
 * Raw syscall() exposed to user programs via <unistd.h> -- PoC-OS's own
 * ABI (see kernel/src/syscall.h): rax = syscall number, up to three
 * arguments in rdi/rsi/rdx, `int $0x80`, return value in rax. Unlike
 * Linux's 6-argument convention, three is the most any PoC-OS syscall
 * ever takes, so unlike sysdeps/demo's bits/syscall.h (which supports
 * 0-7 for RISC-V's `ecall` convention) this only needs 0-3.
 */
#ifndef _MLIBC_SYSCALL_H
#define _MLIBC_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef long __sc_word_t;

/* Implemented in syscall.cpp. */
__sc_word_t __do_syscall0(long);
__sc_word_t __do_syscall1(long, __sc_word_t);
__sc_word_t __do_syscall2(long, __sc_word_t, __sc_word_t);
__sc_word_t __do_syscall3(long, __sc_word_t, __sc_word_t, __sc_word_t);
long __do_syscall_ret(unsigned long);

#ifdef __cplusplus
extern "C++" {

inline long syscall(long n) {
	return __do_syscall_ret(__do_syscall0(n));
}
template<typename Arg0>
long syscall(long n, Arg0 a0) {
	return __do_syscall_ret(__do_syscall1(n, (long)a0));
}
template<typename Arg0, typename Arg1>
long syscall(long n, Arg0 a0, Arg1 a1) {
	return __do_syscall_ret(__do_syscall2(n, (long)a0, (long)a1));
}
template<typename Arg0, typename Arg1, typename Arg2>
long syscall(long n, Arg0 a0, Arg1 a1, Arg2 a2) {
	return __do_syscall_ret(__do_syscall3(n, (long)a0, (long)a1, (long)a2));
}

} /* extern C++ */
#else

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"

#define __scc(x) ((__sc_word_t)(x))
#define __syscall0(n) __do_syscall0(n)
#define __syscall1(n,a) __do_syscall1(n,__scc(a))
#define __syscall2(n,a,b) __do_syscall2(n,__scc(a),__scc(b))
#define __syscall3(n,a,b,c) __do_syscall3(n,__scc(a),__scc(b),__scc(c))
#define __SYSCALL_NARGS_X(a,b,c,d,n,...) n
#define __SYSCALL_NARGS(...) __SYSCALL_NARGS_X(__VA_ARGS__,3,2,1,0,)
#define __SYSCALL_CONCAT_X(a,b) a##b
#define __SYSCALL_CONCAT(a,b) __SYSCALL_CONCAT_X(a,b)
#define __SYSCALL_DISP(b,...) __SYSCALL_CONCAT(b,__SYSCALL_NARGS(__VA_ARGS__))(__VA_ARGS__)
#define __syscall(...) __SYSCALL_DISP(__syscall,__VA_ARGS__)
#define syscall(...) __do_syscall_ret(__syscall(__VA_ARGS__))

#pragma GCC diagnostic pop

#endif

#ifdef __cplusplus
}
#endif

#endif /* _MLIBC_SYSCALL_H */
