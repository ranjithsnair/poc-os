/* CPU exception (ISR) and PIC interrupt (IRQ) dispatch. */
#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/* Register snapshot built by the asm common stubs (asm_stubs.S) before
 * they call into C. Field order matches exactly what the stubs push/the
 * CPU pushes, in address order starting at the stack pointer passed as
 * the struct pointer -- see the push-order comment in asm_stubs.S. */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct registers *regs);

/* Installs IDT gates 0-31 (CPU exceptions) / 32-47 (PIC IRQs) pointing
 * at the asm stubs. Called from idt_init(), not directly. */
void isr_install(void);
void irq_install(void);

/* Registers a C handler for IRQ `irq` (0-15, the PIC's numbering, not
 * the remapped vector); irq_common_handler looks this table up after
 * decoding the vector and before acknowledging the PIC. */
void irq_register_handler(uint8_t irq, irq_handler_t handler);

#endif
