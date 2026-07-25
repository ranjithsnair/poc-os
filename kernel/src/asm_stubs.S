/*
 * Assembly glue that C can't express directly:
 *  - gdt_flush/tss_flush/idt_flush: load the GDTR/IDTR and reload the
 *    segment registers / task register, which requires `lgdt`/`lidt`/
 *    `ltr` and (for CS) a far jump/return -- none of which have a C
 *    spelling.
 *  - isr0-31 / irq0-15: one tiny stub per vector, needed because the CPU
 *    doesn't tell a handler which vector fired it; each stub hardcodes
 *    its own vector number (and a dummy error code, for the exceptions
 *    that don't get one from the CPU) and falls into a shared common
 *    stub that saves registers and calls the real C handler.
 *
 * See the "struct registers" field order in isr.h -- PUSH_ALL/POP_ALL
 * below exist to produce/consume exactly that layout on the stack.
 */
.section .text

.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    mov $0x10, %ax      /* GDT_KERNEL_DATA */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    /* CS can't be loaded via `mov`; a far return to the next instruction
     * is the standard way to reload it in long mode. */
    pushq $0x08         /* GDT_KERNEL_CODE */
    lea 1f(%rip), %rax
    pushq %rax
    lretq
1:
    ret

.global tss_flush
tss_flush:
    mov $0x28, %ax      /* GDT_TSS */
    ltr %ax
    ret

.global idt_flush
idt_flush:
    lidt (%rdi)
    ret

/* Pushes rax..r15 in the order struct registers expects them consumed
 * (i.e. so the LAST push, r15, ends up at the lowest address -- see the
 * offset walk in isr.h/isr.c). */
.macro PUSH_ALL
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro POP_ALL
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

.extern isr_common_handler
.extern irq_common_handler

/* Landing pad every isrN stub below jumps to: saves every general
 * register (so isr_common_handler sees the full interrupted context),
 * calls into C, restores those same registers, discards the vector/
 * error-code pair the stub pushed, and `iretq`s back to wherever (or
 * whatever) was interrupted. */
isr_common_stub:
    PUSH_ALL
    mov %rsp, %rdi        /* struct registers * -> first SysV arg */
    call isr_common_handler
    POP_ALL
    add $16, %rsp         /* discard the vector/error_code pushed below */
    iretq

/* Same shape as isr_common_stub, for hardware IRQs (irq0-15) instead of
 * CPU exceptions. */
irq_common_stub:
    PUSH_ALL
    mov %rsp, %rdi
    call irq_common_handler
    POP_ALL
    add $16, %rsp
    iretq

/* Vectors with no CPU-pushed error code: push a dummy 0 so every vector
 * has the same stack shape by the time isr_common_stub runs. */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common_stub
.endm

/* Vectors where the CPU itself pushes an error code before the return
 * frame -- pushing our own would misalign everything after it. */
.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* IRQs never carry a CPU error code; `vector` is the remapped vector
 * (32-47, see pic.c), not the raw IRQ number. */
.macro IRQ irqnum, vector
.global irq\irqnum
irq\irqnum:
    pushq $0
    pushq $\vector
    jmp irq_common_stub
.endm

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* int 0x80 syscall gate entry. Shares the same register-save shape and
 * struct registers layout as isr_common_stub/irq_common_stub (PUSH_ALL
 * above), just dispatched to a different C handler; there's no CPU
 * error code for a software interrupt, so push a dummy 0 like the
 * no-error ISR stubs do. */
.extern syscall_dispatch

syscall_common_stub:
    PUSH_ALL
    mov %rsp, %rdi
    call syscall_dispatch
    POP_ALL
    add $16, %rsp
    iretq

.global syscall_stub
syscall_stub:
    pushq $0
    pushq $0x80
    jmp syscall_common_stub

/* Freestanding kernel: no GNU stack note means some linkers assume an
 * executable stack segment is needed. Assert it isn't. */
.section .note.GNU-stack,"",@progbits
