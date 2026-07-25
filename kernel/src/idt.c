/*
 * Interrupt Descriptor Table: maps each of the 256 possible interrupt
 * vectors to a handler entry point. Vectors 0-31 are CPU exceptions
 * (architecturally fixed meanings, e.g. 14 = page fault); 32-255 are
 * free for the kernel to assign, and pic.c remaps the 8259 PICs so IRQs
 * 0-15 land on vectors 32-47 instead of colliding with the exceptions.
 */
#include <stdint.h>
#include "idt.h"
#include "gdt.h"
#include "isr.h"
#include "syscall.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idtr idtr;

extern void idt_flush(uint64_t idtr_addr);

/* `ist` selects one of the TSS's 7 alternate stacks (0 = use whatever
 * stack was already active); isr_install() sets ist=1 only for #DF, so
 * a fault on a corrupted kernel stack still runs on a known-good one.
 * `type_attr` 0x8E = present, DPL0, 64-bit interrupt gate -- an
 * interrupt gate (vs. a trap gate) clears IF on entry, so the handler
 * doesn't get re-interrupted before it can save state. */
void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low = addr & 0xFFFF;
    idt[vector].selector = GDT_KERNEL_CODE;
    idt[vector].ist = ist;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

/* Builds the whole IDT and loads it into the CPU. Call once, at boot,
 * after gdt_init() (every gate below points at a kernel code selector
 * gdt_init() must have already created). */
void idt_init(void) {
    isr_install();
    irq_install();
    syscall_install();

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    idt_flush((uint64_t)&idtr);
}
