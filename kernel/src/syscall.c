/*
 * Syscall dispatch for the int 0x80 gate (asm_stubs.S: syscall_stub /
 * syscall_common_stub). Deliberately minimal -- SYS_WRITE_CHAR takes its
 * argument by value specifically to avoid needing to trust/validate a
 * user-supplied pointer this early (no filesystem/VFS to route a real
 * write() to yet, either).
 */
#include <stdint.h>
#include "syscall.h"
#include "idt.h"
#include "isr.h"
#include "process.h"
#include "serial.h"

extern void syscall_stub(void);

static void sys_write_char(struct registers *regs) {
    serial_putc((char)regs->rdi);
    regs->rax = 0;
}

/* Terminates the calling process and reschedules in its place -- *regs
 * gets overwritten with whatever process.c picks next, so the caller's
 * POP_ALL+iretq (syscall_common_stub) never resumes the exited one. */
static void sys_exit(struct registers *regs) {
    process_exit_current(regs);
}

/* Called by syscall_common_stub. */
void syscall_dispatch(struct registers *regs) {
    switch (regs->rax) {
        case SYS_WRITE_CHAR:
            sys_write_char(regs);
            break;
        case SYS_EXIT:
            sys_exit(regs);
            break;
        default:
            serial_print("PoC-OS: unknown syscall number, ignoring.\n");
            regs->rax = (uint64_t)-1;
            break;
    }
}

void syscall_install(void) {
    /* 0xEE = present, DPL3, 64-bit interrupt gate -- DPL3 is what lets
     * ring3 code execute `int 0x80` at all; every other gate in this
     * kernel is DPL0 and would fault if user code tried it. */
    idt_set_gate(0x80, syscall_stub, 0, 0xEE);
}
