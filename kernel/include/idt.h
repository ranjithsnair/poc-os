/* Interrupt Descriptor Table. */
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void idt_init(void);

/* Installs one gate. `ist` selects a TSS alternate stack (0 = none);
 * `type_attr` is the raw present|DPL|type byte -- 0x8E for a kernel-only
 * (DPL0) interrupt gate, 0xEE for one ring3 code can enter directly
 * (e.g. a syscall gate) via `int`. Exposed so isr.c and syscall.c can
 * both install gates without idt.c needing to know about either. */
void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr);

#endif
