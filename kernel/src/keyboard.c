/*
 * Minimal PS/2 keyboard driver: decodes scancode set 1 make codes to US
 * QWERTY ASCII (shifted or not) and feeds them to console.c's line
 * discipline (which echoes them and buffers completed lines for
 * SYS_READ on fd 0). Left/Right Shift and Left Ctrl are tracked (their
 * own make/break codes) -- Ctrl just enough to synthesize the control
 * bytes a real tty produces for Ctrl-<letter> (1-26, e.g. Ctrl-C ->
 * 0x03), since console.c's Ctrl-C handling depends on actually receiving
 * that byte. No Caps Lock, AltGr, or numpad-as-arrows handling.
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

/* Same layout as above, but each key's shifted US QWERTY character --
 * letters uppercase, number row/punctuation shifted (e.g. '.' -> '>').
 * 0 means "same as unshifted" (space, Enter, Tab, Backspace, Esc, and
 * every non-ASCII scancode), not "no character". */
static const char scancode_to_ascii_shifted[128] = {
    /* 0x00 */ 0, 0, '!', '@', '#', '$', '%', '^',
    /* 0x08 */ '&', '*', '(', ')', '_', '+', 0, 0,
    /* 0x10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 0x18 */ 'O', 'P', '{', '}', 0, 0, 'A', 'S',
    /* 0x20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 0x28 */ '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    /* 0x30 */ 'B', 'N', 'M', '<', '>', '?', 0, 0,
    /* 0x38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ 0, 0, 0, 0, 0, 0, 0, 0,
};

#define SCANCODE_LCTRL  0x1D
#define SCANCODE_LSHIFT 0x2A
#define SCANCODE_RSHIFT 0x36

static int ctrl_held = 0;
static int shift_held = 0;

/* IRQ1 handler: reads the one scancode byte the keyboard controller has
 * ready, translates it to ASCII, and hands it to console.c. */
static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode == SCANCODE_LCTRL) {
        ctrl_held = 1;
        return;
    }
    if (scancode == (SCANCODE_LCTRL | 0x80)) {
        ctrl_held = 0;
        return;
    }
    if (scancode == SCANCODE_LSHIFT || scancode == SCANCODE_RSHIFT) {
        shift_held = 1;
        return;
    }
    if (scancode == (SCANCODE_LSHIFT | 0x80) || scancode == (SCANCODE_RSHIFT | 0x80)) {
        shift_held = 0;
        return;
    }
    if (scancode & 0x80) {
        return; /* other key releases; nothing to do without full modifier tracking */
    }

    char base = scancode_to_ascii[scancode];
    if (base == 0) {
        return;
    }

    char c;
    if (ctrl_held && base >= 'a' && base <= 'z') {
        c = (char)(base - 'a' + 1); /* Ctrl-A=0x01 .. Ctrl-Z=0x1A, e.g. Ctrl-C=0x03 -- ignores shift, same as a real tty */
    } else if (shift_held) {
        char shifted = scancode_to_ascii_shifted[scancode];
        c = (shifted != 0) ? shifted : base;
    } else {
        c = base;
    }
    console_feed_char(c);
}

/* Registers keyboard_handler() for IRQ1 and unmasks it at the PIC. */
void keyboard_init(void) {
    irq_register_handler(1, keyboard_handler);
    pic_clear_mask(1);
}
