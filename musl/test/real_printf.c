/* Phase-7 smoke test: real musl printf/vfprintf/stdio (see
 * musl/tools notes / the Makefile's MUSL_STDIO_OBJS), on top of the
 * crt1 + malloc chain already proven by real_hello.c/real_malloc.c.
 * Sticks to integer/string/char conversions - %f/%e/%g/%a print a
 * fixed placeholder instead of a real value (see vfprintf.c's forked
 * fmt_fp(): the kernel doesn't save/restore FPU state across a
 * context switch, so real floating-point codegen is off everywhere in
 * this build, not just here).
 */
#include <stdio.h>

int
main(void)
{
	printf("int=%d str=%s char=%c hex=%x\n", 42, "hello", 'Z', 255);
	printf("float-placeholder: %f\n", 3.5);
	return 0;
}
