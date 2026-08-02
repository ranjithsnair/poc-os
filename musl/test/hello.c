/* Phase-1 smoke test: proves musl's arch/x86_64/syscall_arch.h, forked
 * for poc-os's INT-based syscall convention (see that file's header
 * comment), actually round-trips through the real kernel - no other
 * part of musl is involved yet. Built and linked standalone, the same
 * way poc-os's own user programs are: no _start/crt1, entry point is
 * main() directly.
 */
#include "syscall_arch.h"
#include "bits/syscall.h.in"

int
main(void)
{
	static const char msg[] = "hello from musl syscall_arch on poc-os\n";
	__syscall3(__NR_write, 1, (long)msg, sizeof(msg) - 1);
	__syscall1(__NR_exit, 0);
	for(;;)
		;
}
