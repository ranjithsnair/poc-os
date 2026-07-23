# PoC-OS

A minimal, freestanding x86-64 "hello world" kernel, booted via the
[Limine](https://github.com/limine-bootloader/limine) bootloader. It boots
into 64-bit long mode, clears the screen, draws two lines of text to the
framebuffer using a tiny hand-rolled bitmap font, and logs progress over
the serial port. There's no scheduler, no memory manager, no interrupts —
it's a starting skeleton for OS development, not a functional OS yet.

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
   `boot/kernel` and tells it to use Limine's native boot protocol.
3. Limine loads that ELF file, sets up long mode and paging, maps the
   kernel at the higher-half address `linker.ld` linked it at
   (`0xffffffff80000000`), locates the "requests" the kernel declared
   (see below), and jumps to `kmain` in [kernel/src/main.c](kernel/src/main.c).
4. `kmain` initializes the serial console, checks that Limine understood
   its requests, grabs a framebuffer, and draws to it.

### Limine requests

Limine's boot protocol works by having the kernel place statically
declared "request" structs in a known linker section (`.requests`,
bracketed by `.requests_start_marker` / `.requests_end_marker` — see
[kernel/linker.ld](kernel/linker.ld)). Limine scans the kernel ELF for
these before jumping to `kmain` and fills in each request's `.response`
field. This kernel makes one request: a framebuffer
(`framebuffer_request` in `main.c`). [kernel/src/limine.h](kernel/src/limine.h)
is the vendored protocol header defining these structs/macros — it's
copied verbatim from the Limine project and intentionally left
uncommented/unmodified here.

## Repository layout

```
hello-os/
├── Makefile              top-level build: kernel + Limine -> bootable ISO
├── limine.conf            Limine bootloader configuration
├── kernel/
│   ├── Makefile           compiles kernel/src/*.c -> kernel/bin/kernel
│   ├── linker.ld          memory layout: higher-half link address, segments
│   └── src/
│       ├── main.c         kernel entry point (kmain), framebuffer + font drawing
│       ├── serial.c/.h    polled 16550 UART driver (COM1) used as a debug console
│       ├── font8x8.h      hand-rolled 8x8 bitmap font (only the glyphs used)
│       └── limine.h       vendored Limine boot protocol header (do not edit)
└── limine/                fetched by `make` on demand, not committed
    (Limine bootloader binaries/tools, cloned from limine-bootloader/limine)
```

`kernel/bin/`, `kernel/obj/`, `hello-os.iso`, and `limine/` are all build
outputs / fetched dependencies — none of them are checked into version
control (see `.gitignore`); `make` regenerates them from source.

## Requirements

- `clang` and `ld.lld` (LLVM's linker) — used for cross-compiling
  freestanding, no-red-zone, `mcmodel=kernel` code without needing a
  separately built GCC cross-toolchain.
- `xorriso` — builds the hybrid BIOS/UEFI bootable ISO image.
- `git` — used by the top-level Makefile to fetch the Limine bootloader.
- `qemu-system-x86_64` — to run the kernel in a virtual machine (not
  required just to build it).

On macOS: `brew install llvm xorriso qemu`, then make sure the Homebrew
LLVM's `clang`/`ld.lld` are on `PATH` ahead of Xcode's.

## Building and running

```sh
make        # build kernel/bin/kernel and hello-os.iso
make run    # build (if needed) and boot hello-os.iso in QEMU
```

`make run` passes `-serial stdio`, so kernel log lines (see
`serial_print` calls in `main.c`) print in the same terminal alongside
the QEMU window.

### Cleaning

```sh
make clean       # remove kernel/bin, kernel/obj, hello-os.iso
make distclean   # clean, plus remove the fetched limine/ directory
```

`make distclean` requires network access on the next build, since it
re-clones Limine from GitHub.

## Extending this kernel

This is deliberately the smallest useful skeleton, so the natural next
steps are the usual early-OS-dev milestones: a GDT/IDT and interrupt
handlers, physical/virtual memory management (Limine's memmap and HHDM
requests), a heap allocator, and a proper text renderer/console instead
of two hardcoded `draw_string` calls. Anywhere you add a new Limine
request, declare it the same way `framebuffer_request` is declared in
`main.c` — `static volatile`, in the `.requests` section — so Limine's
scanner can find it before jumping to `kmain`.
