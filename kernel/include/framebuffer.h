/* Public interface for the framebuffer text console -- replaces serial.c
 * as this kernel's only output path. Renders a fixed-width bitmap font
 * onto the linear framebuffer Limine hands us (see LIMINE_FRAMEBUFFER_REQUEST
 * in main.c), with its own cursor and line-wrap/scroll behavior, the same
 * role serial_print()/serial_putc() used to fill. Input still comes from
 * keyboard.c's PS/2 IRQ1 handler, independent of this file. */
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

#include "limine.h"

/* Sets up the text console on top of the given Limine framebuffer
 * (address, pitch, bpp, and the RGB bit layout all come from it -- see
 * struct limine_framebuffer). Clears the screen and resets the cursor to
 * (0, 0). Call once at boot, before anything else tries to print. */
void fb_init(struct limine_framebuffer *fb);

/* Draws one character at the cursor and advances it, wrapping to the next
 * line (scrolling the console up if already on the last line) at the
 * right edge. '\n' moves to the next line, '\b' erases the previous
 * character, '\t' advances to the next multiple-of-8 column -- the same
 * set console.c/serial.c used to special-case. */
void fb_putc(char c);

/* Sends a NUL-terminated string through fb_putc() one byte at a time. */
void fb_print(const char *str);

/* Prints an unsigned 64-bit value in decimal, no leading zeros (0 itself
 * prints as "0"). */
void fb_print_dec(uint64_t v);

#endif
