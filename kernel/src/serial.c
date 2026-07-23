/*
 * Minimal 16550-compatible UART driver for the COM1 serial port.
 * Used only as a debug console (see serial_print calls in main.c) — it has
 * nothing to do with booting or the framebuffer, it just gives us a way to
 * see log output in QEMU (via `-serial stdio`) before/alongside video.
 */
#include "serial.h"

/* Standard x86 I/O port base address for the first serial port (COM1). */
#define COM1 0x3f8

/* Write one byte to an x86 I/O port using the `out` instruction.
 * "a"(val) forces val into AL/AX, "Nd"(port) allows the port to be
 * encoded as an 8-bit immediate (if it fits) or placed in DX. */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Read one byte from an x86 I/O port using the `in` instruction. */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Program the UART: disable interrupts, set the baud rate divisor via
 * DLAB, select the 8N1 frame format, and enable/flush the FIFOs. */
void serial_init(void) {
    outb(COM1 + 1, 0x00); /* IER: disable all interrupts, we only poll */
    outb(COM1 + 3, 0x80); /* LCR: set DLAB=1 so COM1+0/+1 become the divisor */
    outb(COM1 + 0, 0x03); /* divisor low byte: 3 -> 115200/3 = 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte (0 -> only the low byte matters) */
    outb(COM1 + 3, 0x03); /* LCR: DLAB=0, 8 data bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xc7); /* FCR: enable FIFOs, clear rx/tx, 14-byte trigger */
    outb(COM1 + 4, 0x0b); /* MCR: assert RTS/DSR and OUT2 (needed for IRQs) */
}

/* Line Status Register bit 5 (0x20) is set when the transmit holding
 * register is empty, i.e. it is safe to write the next byte. */
static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

/* Send a single byte, spin-waiting until the UART is ready for it. */
void serial_putc(char c) {
    while (!transmit_empty()) { }
    outb(COM1, (uint8_t)c);
}

/* Send a C string byte by byte. Serial terminals expect CRLF line
 * endings, so every '\n' is preceded by an explicit '\r'. */
void serial_print(const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            serial_putc('\r');
        }
        serial_putc(str[i]);
    }
}
