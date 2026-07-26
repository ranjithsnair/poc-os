/*
 * 8259 PIC driver. Every IRQ starts masked after pic_remap(); individual
 * drivers (pit.c, keyboard.c) unmask their own line once they've
 * registered a handler, so nothing can fire into an empty dispatch slot.
 */
#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10
#define ICW4_8086 0x01
#define PIC_EOI   0x20

void pic_remap(void) {
    /* ICW1: start initialization, tell it to expect an ICW4 later. */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    /* ICW2: vector offsets -- master's IRQ0 becomes vector 32, slave's
     * IRQ8 (IRQ0 from its own perspective) becomes vector 40. */
    outb(PIC1_DATA, 32); io_wait();
    outb(PIC2_DATA, 40); io_wait();
    /* ICW3: wiring between the two chips -- master has a slave on the
     * IRQ2 pin (bitmask 0x04); slave identifies itself as cascade 2. */
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();
    /* ICW4: 8086 mode (as opposed to obsolete 8080 mode). */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask everything; drivers unmask their own IRQ once they're ready
     * to receive it (see pic_clear_mask calls in pit.c/keyboard.c). */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* "End Of Interrupt": tells the PIC the current interrupt has been
 * handled so it's free to send the next one. Slave-chip IRQs (8-15)
 * need an EOI sent to *both* chips, since they reach the CPU by being
 * relayed through the master. */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

/* Masks (blocks) one IRQ line so the PIC stops delivering it, without
 * touching any of the other 7 lines on the same chip -- each PIC has
 * one "mask" byte, one bit per line, so this only ever flips a single bit. */
void pic_set_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) | (1 << line)));
}

/* Unmasks (allows) one IRQ line. Called by a driver once its handler is
 * registered and it's ready to actually receive that interrupt. */
void pic_clear_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1 << line)));
}
