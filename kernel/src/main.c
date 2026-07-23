/*
 * PoC-OS kernel entry point.
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
#include "gdt.h"        /* Global Descriptor Table + TSS */
#include "idt.h"        /* Interrupt Descriptor Table */
#include "pic.h"        /* 8259 PIC remap/EOI */
#include "pit.h"        /* timer tick source (IRQ0) */
#include "console.h"    /* stdin line discipline fed by the keyboard IRQ */
#include "keyboard.h"   /* PS/2 keyboard (IRQ1) */
#include "pmm.h"        /* physical frame allocator */
#include "vmm.h"        /* virtual memory mapping */
#include "heap.h"       /* kmalloc/kfree */
#include "process.h"    /* preemptive round-robin scheduler */
#include "tarfs.h"      /* initrd (ustar archive) reader */

/* The ring3 test program (user_test.S): pure register/immediate code
 * with no absolute-address references, safe to copy to a fresh page and
 * run from anywhere -- see process_create()'s doc comment. */
extern const uint8_t user_test_program[];
extern const uint8_t user_test_program_end[];

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

/* Request struct asking Limine for its memory map -- which physical
 * ranges are usable RAM vs. reserved/ACPI/MMIO/the kernel image itself.
 * pmm.c builds the frame allocator's bitmap from this. */
__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

/* Request struct asking Limine for its higher-half direct map offset --
 * the virtual offset at which it identity-maps all physical memory, so
 * the kernel has some way to access physical frames it didn't link
 * against directly. pmm.c needs this to reach the frames it manages. */
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

/* Request struct asking Limine for the modules listed via module_path in
 * limine.conf -- the initrd (an initrd.tar built by the top-level
 * Makefile from initrd/ contents, since there's no disk driver to load files
 * from anywhere else yet). tarfs.c reads it. */
__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
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
 * hits a condition it can't recover from, before interrupts are set up
 * (once they are, this same loop doubles as the idle loop -- see the
 * bottom of kmain -- since `hlt` wakes on every IRQ, runs it via the IDT,
 * then falls through to `hlt` again). */
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
    serial_print("PoC-OS: kernel entered via Limine.\n");

    /* Bring up our own GDT/TSS and IDT before touching anything else --
     * Limine's own GDT isn't guaranteed to stay valid, and every IDT gate
     * names a code selector from ours. pic_remap() moves IRQs 0-15 off
     * vectors 8-15 (which would otherwise collide with CPU exceptions)
     * before pit_init()/keyboard_init() register handlers and unmask
     * their own lines. Interrupts stay off (no `sti` yet) until every
     * gate is actually installed. */
    gdt_init();
    idt_init();
    pic_remap();
    pit_init(100);
    console_init();
    keyboard_init();
    serial_print("PoC-OS: GDT/IDT/PIC/PIT/keyboard initialized.\n");

    /* LIMINE_BASE_REVISION_SUPPORTED becomes false if the bootloader that
     * loaded us is too old to understand the base revision we declared
     * above; continuing would risk relying on protocol features it
     * doesn't implement, so we stop instead. */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        serial_print("PoC-OS: unsupported Limine base revision, halting.\n");
        hcf();
    }

    /* response == NULL means Limine didn't understand/service the
     * request at all; framebuffer_count < 1 means it understood the
     * request but found no usable display. Either way we have nowhere
     * to draw, so bail out to the halt loop. */
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        serial_print("PoC-OS: no framebuffer available, halting.\n");
        hcf();
    }

    /* Both are required to build the physical frame allocator (the
     * memory map says which frames are usable; the HHDM offset is the
     * only way to address them before the kernel manages its own page
     * tables), so there's nothing to do but stop if either is missing. */
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        serial_print("PoC-OS: no memory map/HHDM available, halting.\n");
        hcf();
    }
    pmm_init(memmap_request.response, hhdm_request.response->offset);
    serial_print("PoC-OS: physical memory: ");
    serial_print_dec(pmm_free_frames() * PMM_FRAME_SIZE / (1024 * 1024));
    serial_print(" MiB free of ");
    serial_print_dec(pmm_total_frames() * PMM_FRAME_SIZE / (1024 * 1024));
    serial_print(" MiB total.\n");

    vmm_init(hhdm_request.response->offset);
    serial_print("PoC-OS: VMM initialized.\n");

    /* Exercises kmalloc/kfree once at boot -- a bad heap implementation
     * (e.g. a wrong size computation) tends to show up immediately as a
     * page fault or corrupted metadata on the very first alloc/free, so
     * this doubles as a quick self-test. */
    heap_init();
    void *heap_test = kmalloc(128);
    if (heap_test != NULL) {
        serial_print("PoC-OS: heap allocator initialized (test allocation ok).\n");
        kfree(heap_test);
    } else {
        serial_print("PoC-OS: heap allocator initialized (test allocation FAILED).\n");
    }

    /* Reads a known file back out of the initrd as a smoke test --
     * there's no disk driver yet, so module_request.response->modules[0]
     * (an initrd.tar built from initrd/ contents by the top-level Makefile) is
     * the only "filesystem" this kernel can see. */
    if (module_request.response == NULL || module_request.response->module_count < 1) {
        serial_print("PoC-OS: no initrd module available.\n");
    } else {
        struct limine_file *initrd = module_request.response->modules[0];
        tarfs_init((const uint8_t *)initrd->address, initrd->size);

        uint64_t file_size = 0;
        const uint8_t *file_data = tarfs_read("hello.txt", &file_size);
        if (file_data != NULL) {
            serial_print("PoC-OS: initrd: read hello.txt (");
            serial_print_dec(file_size);
            serial_print(" bytes): ");
            for (uint64_t i = 0; i < file_size; i++) {
                serial_putc((char)file_data[i]);
            }
            serial_print("\n");
        } else {
            serial_print("PoC-OS: initrd: hello.txt not found.\n");
        }
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
    draw_string(fb, 32, 96, "BOOTED VIA LIMINE", 0x00e0e0e0, 2);

    /* Mirror confirmation of a successful boot to the serial log. */
    serial_print("Hello, World! PoC-OS is up.\n");

    /* Enabling IF here (not earlier) means every gate is already
     * installed and every IRQ source already masked-off at the PIC
     * except the ones we explicitly armed above, so nothing can fire
     * into a half-configured IDT. Typing in the QEMU window should now
     * echo characters to this serial log via keyboard.c. */
    asm volatile ("sti");

    /* Spawns two independent copies of the same tiny ring3 test program
     * (each gets its own address space, so this also demonstrates
     * process isolation) and hands the PIT's tick callback to the
     * scheduler. There's no ELF loader yet -- process_create() takes an
     * in-memory code blob, since there's no filesystem to load one from
     * -- so this is the whole "process management" story for now. */
    uint64_t user_program_size = (uint64_t)(user_test_program_end - user_test_program);
    process_create(user_test_program, user_program_size, 0x400000);
    process_create(user_test_program, user_program_size, 0x400000);
    pit_set_tick_callback(scheduler_tick);
    serial_print("PoC-OS: scheduler armed, idling until the first tick.\n");
    hcf();
}
