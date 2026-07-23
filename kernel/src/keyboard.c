/*
 * Minimal PS/2 keyboard driver: decodes scancode set 1 make codes to
 * unshifted US QWERTY ASCII and feeds them to console.c's line
 * discipline (which echoes them and buffers completed lines for
 * SYS_READ on fd 0) -- there's no shift/modifier tracking or
 * framebuffer console input yet.
 */
#include <stdint.h>
#include "keyboard.h"
#include "io.h"
#include "isr.h"
#include "pic.h"
#include "console.h"

#define KEYBOARD_DATA_PORT 0x60

/* Indexed by scancode set 1 make code (bit 7 clear). Break codes (make
 * code | 0x80, sent on key release) are ignored below. 0 marks scancodes
 * with no plain-ASCII mapping (modifiers, F-keys, arrows, numpad, ...). */
static const char scancode_to_ascii[128] = {
    /* 0x00 */ 0, 27, '1', '2', '3', '4', '5', '6',
    /* 0x08 */ '7', '8', '9', '0', '-', '=', '\b', '\t',
    /* 0x10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 0x18 */ 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    /* 0x20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 0x28 */ '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    /* 0x30 */ 'b', 'n', 'm', ',', '.', '/', 0, '*',
    /* 0x38 */ 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 0x40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ 0, 0, 0, 0, 0, 0, 0, 0,
};

static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    if (scancode & 0x80) {
        return; /* key release; nothing to do without modifier tracking */
    }
    char c = scancode_to_ascii[scancode];
    if (c != 0) {
        console_feed_char(c);
    }
}

void keyboard_init(void) {
    irq_register_handler(1, keyboard_handler);
    pic_clear_mask(1);
}
