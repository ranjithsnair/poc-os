/* Public interface for the COM1 serial driver, used as a debug console. */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>  /* fixed-width integer types (uint8_t, etc.) */
#include <stddef.h>  /* size_t */

/* Configure the COM1 UART (baud rate, frame format, FIFOs). Call once at boot. */
void serial_init(void);

/* Block until the transmit holding register is empty, then send one byte. */
void serial_putc(char c);

/* Send a NUL-terminated string over COM1, translating '\n' to "\r\n". */
void serial_print(const char *str);

#endif
