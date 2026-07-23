/*
 * Minimal canonical-mode line discipline sitting between the keyboard
 * IRQ handler and SYS_READ on fd 0. There's no real TTY layer (no
 * termios, no job control) -- just enough buffering that a line typed
 * at the QEMU/serial terminal becomes available to a process a whole
 * line at a time, once Enter is pressed.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);

/* Called by keyboard.c's IRQ1 handler for every decoded ASCII byte.
 * Handles backspace (pops the in-progress line and echoes a destructive
 * backspace) and echoes everything else back out over serial as it's
 * typed; '\n' finalizes the in-progress line, making it available to
 * console_read_nonblock(). */
void console_feed_char(char c);

/* Copies up to len bytes of already-finalized input into buf, consuming
 * them. Returns the number of bytes copied, which is 0 if no finalized
 * line data is available yet -- this never blocks, so it's safe to call
 * from SYS_READ's syscall-context handler (see the comment in
 * syscall.c about why a blocking read would deadlock there). */
uint64_t console_read_nonblock(uint8_t *buf, uint64_t len);

#endif
