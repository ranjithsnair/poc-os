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
#include "fat32.h"      /* fat32_read() -- loading /hellolib's ELF image off disk */
#include "syscall.h"    /* O_RDONLY */
#include "fpu.h"        /* x87/SSE state (enable at boot, save/restore per process) */

/* Declares the Limine base revision marker. Placed in the ".requests"
 * linker section (see linker.ld) so the bootloader can find it by
 * scanning between the start/end markers below. Revision 3 is the
 * protocol revision this kernel was written against. */
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

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
     * before pit_init()/keyboard_init()/serial_input_init() register
     * handlers and unmask their own lines. Interrupts stay off (no `sti`
     * yet) until every gate is actually installed. */
    gdt_init();
    idt_init();
    pic_remap();
    pit_init(100);
    console_init();
    keyboard_init();
    serial_input_init();
    fpu_init();
    serial_print("PoC-OS: GDT/IDT/PIC/PIT/keyboard/serial-input initialized.\n");

    /* LIMINE_BASE_REVISION_SUPPORTED becomes false if the bootloader that
     * loaded us is too old to understand the base revision we declared
     * above; continuing would risk relying on protocol features it
     * doesn't implement, so we stop instead. */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        serial_print("PoC-OS: unsupported Limine base revision, halting.\n");
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

    /* Mirror confirmation of a successful boot to the serial log. */
    serial_print("Hello, World! PoC-OS is up.\n");

    /* Enabling IF here (not earlier) means every gate is already
     * installed and every IRQ source already masked-off at the PIC
     * except the ones we explicitly armed above, so nothing can fire
     * into a half-configured IDT. Typing in the QEMU window should now
     * echo characters to this serial log via keyboard.c. */
    asm volatile ("sti");

    /* Phase 4: boot straight into /hellolib as init -- the mlibc-linked
     * hello_libc.elf built by the top-level Makefile's disk.img rule,
     * installed onto the FAT32 disk by the same build. Loading it here
     * (rather than execve()-ing into it from a tiny trampoline blob, the
     * way every earlier phase's verification programs did) means
     * process_create_from_elf() itself has to do what SYS_EXECVE normally
     * does: read the whole file into a kernel buffer for elf_load() to
     * copy out of (fat32-backed files aren't already resident in RAM the
     * way the old tarfs initrd was), then free that buffer once loading
     * is done. */
    struct fat32_file init_file;
    if (!vfs_open("/", "/hellolib", O_RDONLY, &init_file) || init_file.is_dir) {
        serial_print("PoC-OS: /hellolib not found on the FAT32 disk -- cannot boot into init.\n");
        hcf();
    }
    uint8_t *init_data = (uint8_t *)kmalloc(init_file.size);
    if (init_data == NULL || fat32_read(&init_file, 0, init_data, init_file.size) != (int64_t)init_file.size) {
        serial_print("PoC-OS: failed to read /hellolib off the FAT32 disk.\n");
        hcf();
    }

    const char *argv[] = {"hellolib"};
    const char *envp[] = {"PATH=/", "HOME=/", "TERM=dumb"};
    uint64_t init_pid = process_create_from_elf(init_data, init_file.size,
                                                 1, argv, 3, envp);
    kfree(init_data); /* elf_load() has already copied whatever it needs into the new address space's own frames */
    if (init_pid == 0) {
        serial_print("PoC-OS: failed to start /hellolib as init.\n");
        hcf();
    }
    /* Ctrl-C should reach init (and whatever it's running), not sit
     * unrouted -- see console.c's single-foreground-pid model. */
    console_set_foreground_pid(init_pid);

    pit_set_tick_callback(scheduler_tick);
    serial_print("PoC-OS: scheduler armed, booting into init.\n");
    hcf();
}
