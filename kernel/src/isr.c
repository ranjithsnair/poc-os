/*
 * Dispatch for the asm stubs in asm_stubs.S: isr_install()/irq_install()
 * populate the IDT with pointers to those stubs, and isr_common_handler/
 * irq_common_handler are what the stubs call into once they've saved
 * registers. There's no process/signal machinery yet, so a CPU exception
 * is always fatal; IRQs fan out to whatever driver registered itself.
 */
#include <stddef.h>
#include "isr.h"
#include "idt.h"
#include "pic.h"
#include "serial.h"

extern void isr0(void); extern void isr1(void); extern void isr2(void); extern void isr3(void);
extern void isr4(void); extern void isr5(void); extern void isr6(void); extern void isr7(void);
extern void isr8(void); extern void isr9(void); extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void); extern void irq1(void); extern void irq2(void); extern void irq3(void);
extern void irq4(void); extern void irq5(void); extern void irq6(void); extern void irq7(void);
extern void irq8(void); extern void irq9(void); extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

/* Indexed by vector (0-31); vectors Intel leaves reserved are labeled
 * as such since they can still fault under virtualization/microcode. */
static const char *exception_names[32] = {
    "Divide-by-zero", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point exception", "Alignment check", "Machine check",
    "SIMD floating-point exception", "Virtualization exception", "Control protection exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved"
};

static irq_handler_t irq_handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
}

static void print_hex64(uint64_t v) {
    static const char *hex = "0123456789abcdef";
    /* buf[0..1] = "0x", buf[2..17] = 16 hex digits, buf[18] = '\0'. */
    char buf[19] = "0x0000000000000000";
    for (int i = 0; i < 16; i++) {
        buf[17 - i] = hex[(v >> (i * 4)) & 0xF];
    }
    serial_print(buf);
}

/* Called by isr_common_stub for every CPU exception. There's no recovery
 * path yet -- no process to kill, no signal to deliver -- so this just
 * reports what happened over serial and halts for good. */
void isr_common_handler(struct registers *regs) {
    const char *name = regs->vector < 32 ? exception_names[regs->vector] : "Unknown";
    serial_print("\n*** CPU EXCEPTION: ");
    serial_print(name);
    serial_print(" (vector "); print_hex64(regs->vector);
    serial_print(", error "); print_hex64(regs->error_code);
    serial_print(")\nrip="); print_hex64(regs->rip);
    serial_print(" cs="); print_hex64(regs->cs);
    serial_print(" rflags="); print_hex64(regs->rflags);
    serial_print(" rsp="); print_hex64(regs->rsp);
    serial_print(" ss="); print_hex64(regs->ss);
    if (regs->vector == 14) {
        /* CR2 -- the faulting linear address -- is only meaningful for a
         * page fault (#PF); every other vector leaves it stale from
         * whatever last faulted. */
        uint64_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_print("\ncr2="); print_hex64(cr2);
    }
    serial_print("\nSystem halted.\n");
    for (;;) {
        asm volatile ("cli; hlt");
    }
}

/* Called by irq_common_stub for every PIC IRQ. pic.c remaps IRQs 0-15 to
 * vectors 32-47, so the PIC's IRQ number is just vector - 32. */
void irq_common_handler(struct registers *regs) {
    uint8_t irq = (uint8_t)(regs->vector - 32);
    if (irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }
    pic_send_eoi(irq);
}

void isr_install(void) {
    void (*stubs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    for (int i = 0; i < 32; i++) {
        /* #DF (8) runs on IST1 (gdt.c) in case the fault is a blown
         * kernel stack; every other vector uses whatever stack was
         * already active. */
        uint8_t ist = (i == 8) ? 1 : 0;
        idt_set_gate((uint8_t)i, stubs[i], ist, 0x8E);
    }
}

void irq_install(void) {
    void (*stubs[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };
    for (int i = 0; i < 16; i++) {
        idt_set_gate((uint8_t)(32 + i), stubs[i], 0, 0x8E);
    }
}
