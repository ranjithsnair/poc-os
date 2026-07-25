/* Public interface for the COM1 serial driver, used as a debug console. */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>  /* fixed-width integer types (uint8_t, etc.) */
#include <stddef.h>  /* size_t */

/* Configure the COM1 UART (baud rate, frame format, FIFOs). Call once at boot. */
void serial_init(void);

/* Enables the UART's "data available" interrupt and registers an IRQ4
 * handler that feeds received bytes into console.c (see keyboard.c's
 * IRQ1 handler for the PS/2 equivalent) -- this is what lets a plain
 * `-serial stdio` terminal (no graphical window at all) actually type
 * into the guest, not just see its output. Call once at boot, after
 * idt_init()/pic_remap() (same ordering keyboard_init() needs). */
void serial_input_init(void);

/* Block until the transmit holding register is empty, then send one byte. */
void serial_putc(char c);

/* Send a NUL-terminated string over COM1, translating '\n' to "\r\n". */
void serial_print(const char *str);

/* Send an unsigned 64-bit value over COM1 in decimal, no leading zeros
 * (0 itself prints as "0"). */
void serial_print_dec(uint64_t v);

#endif
