/* See fpu.h. */
#include "fpu.h"

uint8_t fpu_default_state[512] __attribute__((aligned(16)));

static inline uint64_t read_cr0(void) {
    uint64_t v;
    asm volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    asm volatile ("mov %0, %%cr0" : : "r"(v));
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    asm volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    asm volatile ("mov %0, %%cr4" : : "r"(v));
}

#define CR0_EM (1ull << 2)
#define CR0_MP (1ull << 1)
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)

void fpu_init(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~CR0_EM; /* EM=0: FPU/SSE instructions are not emulated-trapped */
    cr0 |= CR0_MP;  /* MP=1: WAIT/FPU instructions respect TS (unused here, but the standard setting) */
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);

    asm volatile ("fninit");
    fpu_save(fpu_default_state);
}

void fpu_save(void *area) {
    asm volatile ("fxsave (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const void *area) {
    asm volatile ("fxrstor (%0)" : : "r"(area) : "memory");
}
