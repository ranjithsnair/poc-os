/*
 * x87/SSE FPU state: enabling it at boot, and saving/restoring each
 * process's state across context switches. Real compiled C code touches
 * XMM registers even without any explicit floating-point in the source
 * (the compiler uses them for memcpy/memset and other optimizations by
 * default on x86-64, where SSE2 is part of the baseline ABI) -- every one
 * of this kernel's own hand-written test programs is pure integer
 * assembly and never needed this, but a real mlibc-linked binary faults
 * with "invalid opcode" the moment it does, since FPU/SSE instructions
 * are disabled by default until CR0/CR4 say otherwise.
 */
#ifndef FPU_H
#define FPU_H

#include <stdint.h>

/* Clears CR0.EM, sets CR0.MP/CR4.OSFXSR/CR4.OSXMMEXCPT, and runs FNINIT
 * -- the standard "turn on x87+SSE" sequence. Called once at boot,
 * before any process (whose saved state is captured via
 * fpu_default_state()) ever runs. */
void fpu_init(void);

/* A 512-byte, 16-byte-aligned FXSAVE-format image of freshly
 * FNINIT-ed FPU/SSE state, captured once by fpu_init() -- every new
 * process starts from a copy of this (a zeroed buffer is not
 * necessarily a valid FXSAVE image, so this is what process_spawn()
 * seeds a new process's saved state with, not memset(0)). */
extern uint8_t fpu_default_state[512] __attribute__((aligned(16)));

/* Thin wrappers around FXSAVE/FXRSTOR; `area` must be 16-byte aligned
 * and at least 512 bytes. */
void fpu_save(void *area);
void fpu_restore(const void *area);

#endif
