/* poc-os syscall convention, not Linux's: a software interrupt
 * (T_SYSCALL == 64 == 0x40, see ../../../include/traps.h) rather than
 * the `syscall` instruction, with the syscall number in %rax and up to
 * six arguments in %rdi/%rsi/%rdx/%rcx/%r8/%r9 - the plain SysV C
 * calling convention, unpermuted (see kernel/syscall.c's argint()).
 * Unlike Linux, arg4 stays in %rcx: poc-os's INT handler saves/restores
 * every GPR into the trapframe itself (kernel/trapasm64.asm), so -
 * unlike the `syscall` instruction, which the CPU itself defines to
 * clobber %rcx and %r11 - nothing here clobbers %rcx, and arg4 doesn't
 * need to be shuffled into %r10 the way upstream musl's x86_64 port
 * does.
 *
 * poc-os's kernel doesn't have an errno/-errno convention yet: every
 * syscall handler just returns -1 on any failure (kernel/syscall.c).
 * __syscall_ret (src/internal/syscall_ret.c) treats any return above
 * -4096UL as -errno, so a poc-os -1 currently always reports EPERM
 * regardless of the real cause - a known gap to close by growing the
 * kernel's syscall handlers to return real -errno values, not something
 * fixable from the musl side.
 */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

static __inline long __syscall0(long n)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n) : "memory");
	return ret;
}

static __inline long __syscall1(long n, long a1)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n), "D"(a1) : "memory");
	return ret;
}

static __inline long __syscall2(long n, long a1, long a2)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2)
						  : "memory");
	return ret;
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3) : "memory");
	return ret;
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	unsigned long ret;
	register long r4 __asm__("rcx") = a4;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r4): "memory");
	return ret;
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	unsigned long ret;
	register long r4 __asm__("rcx") = a4;
	register long r5 __asm__("r8") = a5;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r4), "r"(r5) : "memory");
	return ret;
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	unsigned long ret;
	register long r4 __asm__("rcx") = a4;
	register long r5 __asm__("r8") = a5;
	register long r6 __asm__("r9") = a6;
	__asm__ __volatile__ ("int $0x40" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r4), "r"(r5), "r"(r6) : "memory");
	return ret;
}

/* No vDSO on poc-os. */

#define IPC_64 0
