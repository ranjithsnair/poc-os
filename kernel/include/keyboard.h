/* PS/2 keyboard (port 0x60): decodes keypresses and feeds them to
 * console.c's line discipline. See keyboard.c for the scancode table. */
#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Registers the IRQ1 handler and unmasks it at the PIC. Call once at boot. */
void keyboard_init(void);

#endif
