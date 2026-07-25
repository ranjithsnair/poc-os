# PoC-OS

A freestanding x86-64 kernel, booted via the
[Limine](https://github.com/limine-bootloader/limine) bootloader. It boots
into 64-bit long mode, sets up its own GDT/IDT, physical and virtual memory
management, a kernel heap, and a preemptive round-robin scheduler that runs
ring-3 processes in their own address spaces via an `int 0x80` syscall gate.
There's no filesystem beyond a read-only initrd, no ELF loader, and no
`fork`/`exec` yet — see "Extending this kernel" below for what's still
missing on the way to running a real userspace (mlibc, a cross GCC).

```
+----------------------------------------+
|                                        |
|   HELLO, WORLD!                        |
|   BOOTED VIA LIMINE                    |
|                                        |
+----------------------------------------+
```

## How it boots

1. `qemu-system-x86_64` (or real hardware) loads `hello-os.iso` and hands
   control to Limine, per BIOS or UEFI firmware.
2. Limine reads `boot/limine/limine.conf`, which points it at
   `boot/kernel` and `boot/initrd.tar`, and tells it to use Limine's
   native boot protocol.
3. Limine loads the kernel ELF, sets up long mode and paging, maps the
   kernel at the higher-half address `linker.ld` linked it at
   (`0xffffffff80000000`), locates the "requests" the kernel declared
   (see below), and jumps to `kmain` in [kernel/src/main.c](kernel/src/main.c).
4. `kmain` brings up the serial console, its own GDT/TSS/IDT, the 8259
   PIC, PIT timer and PS/2 keyboard, then the physical/virtual memory
   managers and kernel heap, draws to the framebuffer, reads the initrd,
   spawns a couple of ring-3 test processes, and hands off to the
   scheduler.

### Limine requests

Limine's boot protocol works by having the kernel place statically
declared "request" structs in a known linker section (`.requests`,
bracketed by `.requests_start_marker` / `.requests_end_marker` — see
[kernel/linker.ld](kernel/linker.ld)). Limine scans the kernel ELF for
these before jumping to `kmain` and fills in each request's `.response`
field. This kernel requests: a framebuffer, the memory map, the HHDM
(higher-half direct map) offset, and boot modules (the initrd) — all
declared in `main.c`. [kernel/src/limine.h](kernel/src/limine.h) is the
vendored protocol header defining these structs/macros — it's copied
verbatim from the Limine project and intentionally left
uncommented/unmodified here.

## Repository layout

```
poc/
├── Makefile              top-level build: kernel + initrd + Limine -> bootable ISO
├── limine.conf            Limine bootloader configuration
├── initrd/                files bundled into initrd.tar (the boot-time "filesystem")
├── kernel/
│   ├── Makefile           compiles kernel/src/*.c and *.S -> kernel/bin/kernel
│   ├── linker.ld          memory layout: higher-half link address, segments
│   └── src/
│       ├── main.c              kernel entry point (kmain): brings up every subsystem below
│       ├── serial.c/.h         polled 16550 UART driver (COM1), the debug console
│       ├── font8x8.h           hand-rolled 8x8 bitmap font
│       ├── io.h                shared inb/outb/io_wait port I/O helpers
│       ├── gdt.c/.h            GDT + TSS (kernel/user selectors, IST double-fault stack)
│       ├── idt.c/.h            IDT (256 gates)
│       ├── isr.c/.h            CPU exception + IRQ dispatch, struct registers
│       ├── asm_stubs.S         gdt_flush/idt_flush/tss_flush, isr0-31, irq0-15, syscall_stub
│       ├── pic.c/.h            8259 PIC remap/EOI/masking
│       ├── pit.c/.h            PIT timer (IRQ0), tick counter, scheduler tick callback
│       ├── keyboard.c/.h       PS/2 keyboard (IRQ1), scancode-to-ASCII
│       ├── pmm.c/.h            physical frame allocator (bitmap, built from Limine's memmap)
│       ├── vmm.c/.h            per-address-space page table mapping (PML4 per process)
│       ├── heap.c/.h           kmalloc/kfree (implicit free list over a VMM region)
│       ├── syscall.c/.h        int 0x80 dispatch (SYS_WRITE_CHAR, SYS_EXIT)
│       ├── process.c/.h        preemptive round-robin scheduler
│       ├── user_test.S         hand-written ring3 test program run by process.c
│       ├── tarfs.c/.h          read-only USTAR (tar) reader for the initrd
│       └── limine.h            vendored Limine boot protocol header (do not edit)
└── limine/                fetched by `make` on demand, not committed
    (Limine bootloader binaries/tools, cloned from limine-bootloader/limine)
```

`kernel/bin/`, `kernel/obj/`, `hello-os.iso`, `initrd.tar`, and `limine/`
are all build outputs / fetched dependencies — none of them are checked
into version control (see `.gitignore`); `make` regenerates them from
source.

## Requirements

- `clang` and `ld.lld` (LLVM's linker) — used for cross-compiling
  freestanding, no-red-zone, `mcmodel=kernel` code without needing a
  separately built GCC cross-toolchain.
- `xorriso` — builds the hybrid BIOS/UEFI bootable ISO image.
- `tar` — builds the initrd (`--format=ustar`, which `kernel/src/tarfs.c`'s
  parser depends on).
- `git` — used by the top-level Makefile to fetch the Limine bootloader.
- `qemu-system-x86_64` — to run the kernel in a virtual machine (not
  required just to build it).

On macOS: `brew install llvm xorriso qemu`, then make sure the Homebrew
LLVM's `clang`/`ld.lld` are on `PATH` ahead of Xcode's.

## Building and running

```sh
make        # build kernel/bin/kernel, initrd.tar, and hello-os.iso
make run    # build (if needed) and boot hello-os.iso in QEMU
```

`make run` passes `-serial stdio`, so kernel log lines (see
`serial_print` calls throughout `kernel/src/`) print in the same terminal
alongside the QEMU window. A working boot should log GDT/IDT/PIC/PIT/
keyboard, PMM, VMM, and heap initialization, a successful initrd read,
and then process-creation messages as the scheduler starts running the
two ring-3 test processes (each prints "Hi" via SYS_WRITE_CHAR and exits).

### Cleaning

```sh
make clean       # remove kernel/bin, kernel/obj, hello-os.iso, initrd.tar
make distclean   # clean, plus remove the fetched limine/ directory
```

`make distclean` requires network access on the next build, since it
re-clones Limine from GitHub.

## Extending this kernel

Still missing, roughly in the order it'd make sense to build them:

- A real filesystem (the initrd is read-only and entirely in memory) —
  needs a disk/block-device driver (e.g. virtio-blk or AHCI) plus an
  on-disk format (ext2 or FAT32), fronted by a VFS layer.
- An ELF loader and an `exec()`-style syscall, once there's a filesystem
  to load a binary from.
- `fork()` and a real process exit/wait model (`process.c`'s SYS_EXIT
  just frees the slot and reschedules; there's no parent/child
  relationship or exit status to collect yet).
- A TTY layer (line discipline, job control) over the PS/2
  keyboard/framebuffer, needed before anything expects a real terminal.
- A libc port (e.g. [mlibc](https://github.com/managarm/mlibc), which is
  designed for exactly this kind of hobby-OS bring-up) implementing its
  sysdeps layer against this kernel's syscalls.
- A cross-compiled GCC/binutils targeting this kernel (sysroot = the
  libc port above).

Anywhere you add a new Limine request, declare it the same way
`framebuffer_request` is declared in `main.c` — `static volatile`, in the
`.requests` section — so Limine's scanner can find it before jumping to
`kmain`.
