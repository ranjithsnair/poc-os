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
#include "vfs.h"        /* writable FAT32 disk (virtio_blk.c/fat32.c) mount */
#include "fpu.h"        /* x87/SSE state (enable at boot, save/restore per process) */

/* The ring3 test program (user_test.S): pure register/immediate code
 * with no absolute-address references, safe to copy to a fresh page and
 * run from anywhere -- see process_create()'s doc comment. */
extern const uint8_t user_test_program[];
extern const uint8_t user_test_program_end[];

/* fork_test.S / exec_test.S: same flat-blob shape as user_test_program,
 * exercising SYS_FORK/SYS_WAITPID and SYS_EXECVE respectively. */
extern const uint8_t fork_test_program[];
extern const uint8_t fork_test_program_end[];
extern const uint8_t exec_test_program[];
extern const uint8_t exec_test_program_end[];
extern const uint8_t pipe_test_program[];
extern const uint8_t pipe_test_program_end[];
extern const uint8_t signal_test_program[];
extern const uint8_t signal_test_program_end[];
extern const uint8_t tty_test_program[];
extern const uint8_t tty_test_program_end[];
extern const uint8_t mmap_test_program[];
extern const uint8_t mmap_test_program_end[];
extern const uint8_t fat32_test_program[];
extern const uint8_t fat32_test_program_end[];
extern const uint8_t libc_test_program[];
extern const uint8_t libc_test_program_end[];
extern const uint8_t libc_test_gcc_program[];
extern const uint8_t libc_test_gcc_program_end[];
extern const uint8_t bash_test_program[];
extern const uint8_t bash_test_program_end[];

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

/* Boot-time test scaffolding for the TTY layer (see console.c): simulates
 * a physically-typed Ctrl-C a few ticks after boot, since there's no way
 * to inject a real PS/2 scancode in this environment. Registered as the
 * PIT's tick callback instead of scheduler_tick directly so it reliably
 * runs on every timer IRQ regardless of which context got interrupted --
 * unlike code sitting after `hlt` in kmain's own idle loop, which never
 * runs again once the scheduler has handed off to a user process (every
 * later timer IRQ enters via whichever process's own kernel stack is
 * current, never kmain's). This whole function goes away along with the
 * process_create() test-program calls it's paired with, once a real init
 * process exists. */
static void tick_with_injected_ctrl_c(struct registers *regs) {
    static int injected = 0;
    /* Round-robin gives each READY process a full tick's slice before
     * switching to the next, so with 7 boot-spawned test processes
     * ahead of it in the rotation, tty_test_program's first slice (where
     * it registers itself as the foreground pid) doesn't start until
     * roughly tick 6-7 -- comfortably before this, but not so late that
     * it eats into tty_test_program's own ~20-tick wait window. */
    if (!injected && pit_get_ticks() >= 15) {
        console_feed_char(0x03);
        injected = 1;
    }
    scheduler_tick(regs);
}

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
    fpu_init();
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

    /* Reads a known file back out of the initrd as a smoke test -- purely
     * a kernel-internal check now (process-visible files live on the
     * FAT32 disk below; see vfs_init()). module_request.response->modules[0]
     * (an initrd.tar built from initrd/ contents by the top-level Makefile). */
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

    /* Mounts the writable FAT32 disk (virtio_blk.c/fat32.c) that
     * process-visible SYS_OPEN/SYS_EXECVE now go through -- see vfs.c.
     * No fallback if this fails: every test program spawned below that
     * touches a file (SYS_OPEN, SYS_EXECVE) will simply fail its own
     * open/exec call and report that, same as any other missing
     * resource. */
    if (!vfs_init()) {
        serial_print("PoC-OS: no writable filesystem -- file-backed syscalls will fail.\n");
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

    /* Exercise SYS_FORK/SYS_WAITPID and SYS_EXECVE -- expect "CP0" (child,
     * then parent's reaped-status digit) and "OK" (exec_target.elf, loaded
     * from the initrd, running in place of exec_test_program) somewhere
     * in the serial log, interleaved with the "Hi" lines above by the
     * round-robin scheduler. */
    uint64_t fork_test_size = (uint64_t)(fork_test_program_end - fork_test_program);
    process_create(fork_test_program, fork_test_size, 0x400000);
    uint64_t exec_test_size = (uint64_t)(exec_test_program_end - exec_test_program);
    process_create(exec_test_program, exec_test_size, 0x400000);

    /* Exercises SYS_PIPE/SYS_DUP2 (via the fork()-after-pipe() refcounting
     * both process_fork() and process_fd_dup2() need to get right):
     * expect "hi" from the child echoing back what the parent piped it. */
    uint64_t pipe_test_size = (uint64_t)(pipe_test_program_end - pipe_test_program);
    process_create(pipe_test_program, pipe_test_size, 0x400000);

    /* Exercises SYS_SIGACTION/SYS_KILL/SYS_SIGRETURN (a caught signal) and
     * default-action termination: expect "HS" (handler ran, then the
     * interrupted code resumed correctly) followed by "T" (a SIGTERM'd
     * child's exit status came back as the POSIX 128+signum convention). */
    uint64_t signal_test_size = (uint64_t)(signal_test_program_end - signal_test_program);
    process_create(signal_test_program, signal_test_size, 0x400000);

    /* Exercises the TTY layer: SYS_IOCTL's TCGETS/TCSETS/TIOCSPGRP, and
     * Ctrl-C -> SIGINT via console_feed_char() (see
     * tick_with_injected_ctrl_c() below, which simulates the keypress --
     * there's no way to inject a real PS/2 scancode in this environment).
     * Expect "ic!". */
    uint64_t tty_test_size = (uint64_t)(tty_test_program_end - tty_test_program);
    process_create(tty_test_program, tty_test_size, 0x400000);

    /* Exercises SYS_ANON_ALLOCATE/SYS_ANON_FREE now that the latter
     * actually frees physical frames: expect "M" (60 rounds of allocate
     * 4MiB + touch it + free it all succeeded, which 240MiB total would
     * not survive on this VM's ~222MiB if frames were being leaked). */
    uint64_t mmap_test_size = (uint64_t)(mmap_test_program_end - mmap_test_program);
    process_create(mmap_test_program, mmap_test_size, 0x400000);

    /* Exercises the writable FAT32 disk end-to-end: SYS_OPEN(O_CREAT|
     * O_WRONLY), SYS_WRITE, SYS_CLOSE, reopen read-only, SYS_READ back
     * and compare, then SYS_MKDIR + the same dance one directory level
     * down. Expect "WD". */
    uint64_t fat32_test_size = (uint64_t)(fat32_test_program_end - fat32_test_program);
    process_create(fat32_test_program, fat32_test_size, 0x400000);

    /* Exercises the Phase 1 mlibc port: execve()s into a real C program
     * (userland/hello_libc.c) linked against mlibc's libc.a. Expect
     * "hello from libc" to appear verbatim in the serial log. */
    uint64_t libc_test_size = (uint64_t)(libc_test_program_end - libc_test_program);
    process_create(libc_test_program, libc_test_size, 0x400000);

    /* Phase 2 verification: same program, but built with the real cross
     * GCC instead of clang -- see libc_test_gcc.S's doc comment. Expect
     * a second "hello from libc" line. */
    uint64_t libc_test_gcc_size = (uint64_t)(libc_test_gcc_program_end - libc_test_gcc_program);
    process_create(libc_test_gcc_program, libc_test_gcc_size, 0x400000);

    /* Phase 3 verification: execve()s into the cross-compiled real GNU
     * bash binary in isolation (not yet wired as init -- see
     * bash_test.S's doc comment). Expect a bash prompt/startup activity
     * in the serial log, not a fault/reboot. */
    uint64_t bash_test_size = (uint64_t)(bash_test_program_end - bash_test_program);
    process_create(bash_test_program, bash_test_size, 0x400000);

    pit_set_tick_callback(tick_with_injected_ctrl_c);
    serial_print("PoC-OS: scheduler armed, idling until the first tick.\n");
    hcf();
}
