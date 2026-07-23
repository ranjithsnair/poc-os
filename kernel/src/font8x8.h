#ifndef FONT8X8_H
#define FONT8X8_H

#include <stdint.h>  /* uint8_t */
#include <stddef.h>  /* size_t */

/*
 * Minimal 8x8 bitmap font covering only the glyphs this kernel prints.
 * Row bytes read left-to-right: bit 7 (MSB) is the leftmost pixel.
 * Unknown characters fall back to a blank glyph.
 *
 * The table below is a linear array, not indexed by character code, so
 * it only needs one entry per glyph actually used by main.c instead of
 * a full ASCII set — currently that's every letter in "HELLO, WORLD!"
 * and "LUCY-OS BOOTED VIA LIMINE" plus the punctuation they use.
 * Add a row here (and nowhere else) to support a new character.
 */

/* One glyph: the character it represents, and its 8x8 bitmap encoded as
 * 8 bytes (one per row, 8 pixels per byte). */
struct font8x8_glyph {
    char c;
    uint8_t rows[8];
};

/* clang-format-style visual grid: each `0b...` byte is one pixel row,
 * '1' bits are drawn, '0' bits are left as background. */
static const struct font8x8_glyph font8x8_table[] = {
    { 'A', { 0b00111000, 0b01101100, 0b01100110, 0b01100110, 0b01111110, 0b01100110, 0b01100110, 0b00000000 } },
    { 'B', { 0b01111100, 0b01100110, 0b01100110, 0b01111100, 0b01100110, 0b01100110, 0b01111100, 0b00000000 } },
    { 'C', { 0b00111100, 0b01100110, 0b01100000, 0b01100000, 0b01100000, 0b01100110, 0b00111100, 0b00000000 } },
    { 'D', { 0b01111000, 0b01101100, 0b01100110, 0b01100110, 0b01100110, 0b01101100, 0b01111000, 0b00000000 } },
    { 'E', { 0b01111110, 0b01100000, 0b01100000, 0b01111100, 0b01100000, 0b01100000, 0b01111110, 0b00000000 } },
    { 'H', { 0b01100110, 0b01100110, 0b01100110, 0b01111110, 0b01100110, 0b01100110, 0b01100110, 0b00000000 } },
    { 'I', { 0b00111100, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00111100, 0b00000000 } },
    { 'L', { 0b01100000, 0b01100000, 0b01100000, 0b01100000, 0b01100000, 0b01100000, 0b01111110, 0b00000000 } },
    { 'M', { 0b01100110, 0b01111110, 0b01111110, 0b01101110, 0b01100110, 0b01100110, 0b01100110, 0b00000000 } },
    { 'N', { 0b01100110, 0b01110110, 0b01111110, 0b01101110, 0b01100110, 0b01100110, 0b01100110, 0b00000000 } },
    { 'O', { 0b00111100, 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b00111100, 0b00000000 } },
    { 'R', { 0b01111100, 0b01100110, 0b01100110, 0b01111100, 0b01111000, 0b01101100, 0b01100110, 0b00000000 } },
    { 'S', { 0b00111110, 0b01100000, 0b01100000, 0b00111100, 0b00000110, 0b00000110, 0b01111100, 0b00000000 } },
    { 'T', { 0b01111110, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00000000 } },
    { 'U', { 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b00111100, 0b00000000 } },
    { 'V', { 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b01100110, 0b00111100, 0b00011000, 0b00000000 } },
    { 'W', { 0b01000010, 0b01000010, 0b01000010, 0b01000010, 0b01011010, 0b01011010, 0b00100100, 0b00000000 } },
    { 'Y', { 0b01100110, 0b01100110, 0b00111100, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00000000 } },
    { ',', { 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00011000, 0b00011000, 0b00110000 } },
    { '!', { 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00000000, 0b00011000, 0b00000000 } },
    { '-', { 0b00000000, 0b00000000, 0b00000000, 0b01111110, 0b00000000, 0b00000000, 0b00000000, 0b00000000 } },
};

/* Fallback glyph (all pixels off) for characters not in the table above,
 * e.g. the spaces in "LUCY-OS BOOTED VIA LIMINE" — this is what makes
 * spaces render as blank instead of crashing or drawing garbage. */
static const uint8_t font8x8_blank[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* Linear-search the glyph table for character `c` and return its 8 row
 * bytes, or the blank glyph if it isn't present. A linear scan is fine
 * here since the table only has a couple dozen entries and this only
 * runs a handful of times per boot. */
static inline const uint8_t *font8x8_get(char c) {
    for (size_t i = 0; i < sizeof(font8x8_table) / sizeof(font8x8_table[0]); i++) {
        if (font8x8_table[i].c == c) {
            return font8x8_table[i].rows;
        }
    }
    return font8x8_blank;
}

#endif
