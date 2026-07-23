/*
 * Lucy-OS kernel entry point.
 *
 * This kernel is loaded and started by the Limine bootloader, which hands
 * off control at `kmain` in 64-bit long mode with paging already set up
 * (see linker.ld for the higher-half virtual address the kernel expects).
 * All communication with Limine happens through "requests": statically
 * declared structs the bootloader scans for before jumping to the kernel,
 * and then fills in a `.response` pointer on.
 */
#include <stdint.h>   /* fixed-width integer types */
#include <stddef.h>   /* size_t */
#include <stdbool.h>  /* bool */

#include "limine.h"     /* Limine boot protocol structs/macros (vendored) */
#include "serial.h"     /* COM1 debug console */
#include "font8x8.h"    /* bitmap font used to draw text to the framebuffer */

/* Declares the Limine base revision marker. Placed in the ".requests"
 * linker section (see linker.ld) so the bootloader can find it by
 * scanning between the start/end markers below. Revision 3 is the
 * protocol revision this kernel was written against. */
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

/* Request struct asking Limine for a framebuffer to draw into.
 * `.response` starts NULL and is filled in by the bootloader before
 * the kernel is entered if a framebuffer was found. */
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

/* Bracket markers so Limine knows where the block of request structs
 * begins and ends in the .data section; every request must live between
 * these two symbols. */
__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* "Halt and catch fire": park the CPU forever. Used whenever the kernel
 * hits a condition it can't recover from. `hlt` stops the CPU until the
 * next interrupt; since interrupts are never enabled, this loop never
 * actually wakes up — it just avoids running hlt in a tight busy spin. */
static void hcf(void) {
    for (;;) {
        asm ("hlt");
    }
}

/* Write a single pixel into the framebuffer.
 * The framebuffer is a flat array of bytes; `pitch` is the number of
 * bytes per row (which may be larger than width * bytes-per-pixel due
 * to alignment/padding), and `bpp` is bits per pixel (32 here), so
 * bpp / 8 gives the byte size of one pixel. `color` is packed as
 * 0x00RRGGBB to match a 32-bit XRGB framebuffer format. */
static void put_pixel(struct limine_framebuffer *fb, size_t x, size_t y, uint32_t color) {
    uint32_t *pixel = (uint32_t *)((uint8_t *)fb->address + y * fb->pitch + x * (fb->bpp / 8));
    *pixel = color;
}

/* Draw one character glyph at pixel position (x, y), scaled up by an
 * integer factor. font8x8_get() returns 8 rows of 8 bits each; bit 7
 * (0x80) of each row is the leftmost column, so we shift a mask across
 * the byte to test each pixel left to right. Every "on" bit is expanded
 * into a scale x scale block of pixels so the tiny 8x8 font stays
 * legible on a real display. */
static void draw_char(struct limine_framebuffer *fb, size_t x, size_t y, char c, uint32_t color, int scale) {
    const uint8_t *glyph = font8x8_get(c);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        put_pixel(fb, x + col * scale + sx, y + row * scale + sy, color);
                    }
                }
            }
        }
    }
}

/* Draw a NUL-terminated string left to right starting at (x, y).
 * Each glyph is 8 pixels wide before scaling, so the cursor advances by
 * 8 * scale pixels per character (monospaced, no kerning). */
static void draw_string(struct limine_framebuffer *fb, size_t x, size_t y, const char *str, uint32_t color, int scale) {
    size_t cursor = x;
    for (size_t i = 0; str[i] != '\0'; i++) {
        draw_char(fb, cursor, y, str[i], color, scale);
        cursor += 8 * scale;
    }
}

/* Kernel entry point. Named "kmain" to match ENTRY(kmain) in linker.ld. */
void kmain(void) {
    /* Bring up the serial console first so every step below can log,
     * even if the framebuffer path fails. */
    serial_init();
    serial_print("Lucy-OS: kernel entered via Limine.\n");

    /* LIMINE_BASE_REVISION_SUPPORTED becomes false if the bootloader that
     * loaded us is too old to understand the base revision we declared
     * above; continuing would risk relying on protocol features it
     * doesn't implement, so we stop instead. */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        serial_print("Lucy-OS: unsupported Limine base revision, halting.\n");
        hcf();
    }

    /* response == NULL means Limine didn't understand/service the
     * request at all; framebuffer_count < 1 means it understood the
     * request but found no usable display. Either way we have nowhere
     * to draw, so bail out to the halt loop. */
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        serial_print("Lucy-OS: no framebuffer available, halting.\n");
        hcf();
    }

    /* Just use the first framebuffer Limine reports (typically the
     * primary display); this kernel doesn't handle multi-monitor setups. */
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    /* Clear the screen to a dark blue-grey background, one pixel at a
     * time (there's no bulk-fill/memset helper for this yet). */
    for (size_t y = 0; y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            put_pixel(fb, x, y, 0x00101828);
        }
    }

    /* Title line in green at 4x scale (32px-tall glyphs), subtitle in
     * light grey at 2x scale (16px-tall glyphs) beneath it. */
    draw_string(fb, 32, 32, "HELLO, WORLD!", 0x0032cd90, 4);
    draw_string(fb, 32, 96, "LUCY-OS BOOTED VIA LIMINE", 0x00e0e0e0, 2);

    /* Mirror confirmation of a successful boot to the serial log. */
    serial_print("Hello, World! Lucy-OS is up.\n");

    /* Nothing left to do: there's no scheduler, no further kernel work,
     * so park the CPU forever. */
    hcf();
}
