/* Phase-5 smoke test: a real musl program, not a raw-syscall stand-in
 * like musl/test/hello.c - linked against actual musl crt1/
 * __libc_start_main/__init_tls/write()/exit() object code (see
 * musl/tools/build-real-hello notes / the Makefile rule), exercising
 * the full startup chain: kernel/exec.c's execve() builds the
 * argc/argv/envp/auxv stack, crt1's _start reads it, __libc_start_main
 * runs __init_libc/__init_tls (which calls the forked
 * __set_thread_area.s -> SYS_arch_prctl -> the kernel's WRMSR(FS_BASE)
 * path), then calls this main() with real argc/argv/envp, and on
 * return runs musl's real exit() -> _Exit() -> SYS_exit_group.
 */
#include <unistd.h>

int
main(int argc, char **argv, char **envp)
{
	static const char msg[] = "hello from REAL musl (crt1 + libc_start_main) on poc-os\n";
	write(1, msg, sizeof(msg) - 1);
	return argc > 100 ? 1 : 0;  /* always false; just exercises argc */
}
