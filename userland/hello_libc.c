/* Simplest possible mlibc-linked program: proves a normal C program using
 * mlibc's printf() (not a hand-written raw-syscall test) runs on PoC-OS,
 * both statically linked (as /hellolib) and as a dynamically-linked PIE
 * (as /hellodyn) -- see the top-level Makefile and main.c. */
#include <stdio.h>

int main(void) {
    printf("hello from libc\n");
    return 0;
}
