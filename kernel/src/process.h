/*
 * Preemptive round-robin scheduler for ring3 processes. There's no
 * filesystem yet, so process_create() takes an in-memory code blob
 * rather than loading an ELF from disk -- that's the natural extension
 * point once a filesystem exists (an exec() syscall would read the file
 * into a buffer and hand it to the same primitive this uses internally).
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "isr.h"

/* Creates a new ring3 process: its own address space, a code page
 * holding a copy of `code[0..code_size)` mapped executable+user at
 * `entry_virt`, and a stack page mapped immediately after it. The code
 * must be position-independent in the sense user_test.S documents (no
 * absolute-address references), since it's copied to a fresh physical
 * frame and mapped somewhere new. Returns the new PID, or 0 on failure
 * (out of memory, or no free process slots). */
uint64_t process_create(const uint8_t *code, uint64_t code_size, uint64_t entry_virt);

/* Called from pit.c's tick callback: saves the interrupted context (if
 * it belonged to a running process), picks the next READY process
 * round-robin, and overwrites *regs with its saved context -- the
 * caller's existing POP_ALL+iretq then resumes that process instead. */
void scheduler_tick(struct registers *regs);

/* Called from syscall.c's SYS_EXIT handler: marks the currently running
 * process unused and immediately reschedules, the same way, so the
 * caller's POP_ALL+iretq never resumes the now-dead process. */
void process_exit_current(struct registers *regs);

#endif
