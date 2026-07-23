/* int 0x80 syscall gate -- PoC-OS's own ABI, not Linux's. */
#ifndef SYSCALL_H
#define SYSCALL_H

/* Number in rax, args in rdi/rsi/rdx; return value written back into
 * rax (the syscall stub preserves it across the trip back to ring3). */
#define SYS_WRITE_CHAR 0 /* rdi = character to print (by value, not a pointer) */
#define SYS_EXIT       1 /* halts -- there's no process to actually exit yet */

/* Installs the vector-0x80 IDT gate. Called from idt_init(). */
void syscall_install(void);

#endif
