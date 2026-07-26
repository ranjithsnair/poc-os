/* Global Descriptor Table + Task State Segment. */
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Selectors installed by gdt_init(); idt.c uses GDT_KERNEL_CODE when
 * building every interrupt gate (handlers run in kernel code), and the
 * user selectors/TSS are wired up now for when ring3 support arrives. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

void gdt_init(void);

/* Sets RSP0 in the TSS: the stack the CPU switches to on any ring3->ring0
 * transition (interrupt or syscall while running user code). Unused
 * until user mode exists, but lives in the same TSS gdt_init() builds. */
void tss_set_kernel_stack(uint64_t rsp0);

#endif
