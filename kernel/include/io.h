/* Shared x86 port I/O helpers, used by every driver that talks to
 * hardware over `in`/`out` instead of memory-mapped registers (PIC, PIT,
 * PS/2 keyboard). */
#ifndef IO_H
#define IO_H

#include <stdint.h>

/* Writes one byte (8 bits) to a hardware I/O port -- the x86 way of
 * talking to devices like the PIC/PIT/keyboard controller, separate
 * from normal memory reads/writes. */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Reads one byte back from a hardware I/O port. */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Same as outb(), but writes 2 bytes (16 bits) at once. */
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Same as inb(), but reads 2 bytes (16 bits) at once. */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Same as outb(), but writes 4 bytes (32 bits) at once. */
static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

/* Same as inb(), but reads 4 bytes (32 bits) at once. */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Port 0x80 is an unused POST diagnostic port; writing to it is a
 * standard trick to burn ~1us, which the PIC needs between init command
 * words on real hardware (QEMU doesn't care, but real 8259s do). */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
