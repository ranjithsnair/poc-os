/*
 * Global Descriptor Table + Task State Segment.
 *
 * Limine leaves its own GDT in place after handing off control, but per
 * the Limine boot protocol spec that GDT belongs to the bootloader and
 * isn't guaranteed to remain valid once the kernel is running -- so the
 * kernel builds and loads its own before doing anything that names a
 * segment selector, which includes every IDT gate (idt.c).
 *
 * Long mode ignores base/limit for code/data segments (flat memory
 * model), so only the access/flag bits matter for those four entries;
 * the TSS descriptor is the one entry that still needs a real base
 * address, since the CPU reads it directly via `ltr`/`str`.
 */
#include <stdint.h>
#include <stddef.h>
#include "gdt.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 7 slots: null, kernel code, kernel data, user code, user data, plus the
 * two 8-byte halves the 64-bit TSS descriptor needs. */
static uint64_t gdt[7];
static struct gdtr gdtr;
static struct tss tss;

/* Stack IST1 points at -- used only by the #DF (double fault) gate
 * (idt.c/isr.c), so a fault caused by a corrupted or exhausted kernel
 * stack still has a known-good stack to run the handler on. */
static uint8_t double_fault_stack[4096] __attribute__((aligned(16)));

extern void gdt_flush(uint64_t gdtr_addr);
extern void tss_flush(void);

/* Encodes one flat (base=0, limit=0xFFFFF) code/data descriptor. `access`
 * is the standard P|DPL|S|Type byte; `flags` is the top nibble of the
 * granularity byte (G|D/B-or-L|L|AVL). */
static uint64_t make_flat_descriptor(uint8_t access, uint8_t flags) {
    uint64_t limit = 0xFFFFF;
    uint64_t desc = 0;
    desc |= limit & 0xFFFF;
    desc |= (uint64_t)access << 40;
    desc |= (uint64_t)((flags << 4) | ((limit >> 16) & 0x0F)) << 48;
    return desc;
}

/* Fills gdt[5]/gdt[6] with the 16-byte system descriptor a 64-bit TSS
 * needs (twice the width of a code/data descriptor, since it carries a
 * full 64-bit base address). */
static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = 0, high = 0;
    low |= limit & 0xFFFF;
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40; /* present, type=9 (64-bit TSS, not busy) */
    low |= (uint64_t)((limit >> 16) & 0x0F) << 48;
    low |= ((base >> 24) & 0xFF) << 56;
    high = (base >> 32) & 0xFFFFFFFF;
    gdt[5] = low;
    gdt[6] = high;
}

/* Builds the 7-entry GDT above, loads it into the CPU, and loads the TSS
 * on top of it. Must run before idt_init(), since every interrupt gate
 * names one of the code selectors set up here. Call once, at boot. */
void gdt_init(void) {
    gdt[0] = 0; /* null descriptor, required by the architecture */
    gdt[1] = make_flat_descriptor(0x9A, 0xA); /* kernel code: P|S|exec/read, L=1 */
    gdt[2] = make_flat_descriptor(0x92, 0x8); /* kernel data: P|S|read/write */
    gdt[3] = make_flat_descriptor(0xFA, 0xA); /* user code, DPL=3, L=1 */
    gdt[4] = make_flat_descriptor(0xF2, 0x8); /* user data, DPL=3 */

    for (size_t i = 0; i < sizeof(tss); i++) {
        ((uint8_t *)&tss)[i] = 0;
    }
    tss.ist1 = (uint64_t)double_fault_stack + sizeof(double_fault_stack);
    tss.iomap_base = sizeof(tss); /* no I/O bitmap: everything past the TSS is "out of range" */
    set_tss_descriptor((uint64_t)&tss, sizeof(tss) - 1);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)&gdt;

    gdt_flush((uint64_t)&gdtr);
    tss_flush();
}

/* Tells the CPU which kernel stack to switch to on the next ring-3 ->
 * ring-0 privilege change (e.g. a syscall or IRQ firing while a user
 * process is running). Called by process.c every time it switches to
 * a different process, so each one traps into its own kernel stack. */
void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
