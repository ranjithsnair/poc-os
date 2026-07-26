/* 8259 Programmable Interrupt Controller (master + cascaded slave). */
#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* Remaps IRQs 0-15 to vectors 32-47, since their power-on-default
 * vectors (8-15) collide with CPU exceptions. Must run before any IRQ
 * can be safely unmasked. */
void pic_remap(void);

/* Acknowledges IRQ `irq` so the PIC will deliver further interrupts;
 * must be called once per IRQ, after handling it. */
void pic_send_eoi(uint8_t irq);

void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif
