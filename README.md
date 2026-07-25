# PoC-OS

A freestanding x86-64 kernel, booted via the
[Limine](https://github.com/limine-bootloader/limine) bootloader, that boots
into 64-bit long mode, brings up its own GDT/IDT/PIC/PIT, physical and
virtual memory management, a kernel heap, and a preemptive round-robin
scheduler running ring-3 processes via an `int 0x80` syscall gate. It mounts
a writable FAT32 disk over a virtio-blk device, loads real ELF64 executables
off it (static or dynamically linked), and supports `fork`/`execve`/
`waitpid`, pipes, signals, directory listing/unlink/rmdir/rename, and a
line-discipline console -- enough to boot a real, dynamically-linked
[busybox](https://busybox.net/) as `init`, running its `ash` shell as an
interactive terminal (`ls`/`cd`/`pwd`/`cat`/`mkdir`/`rm`/`mv`/`grep`/`find`/
... -- see [toolchain/busybox-pocos.config](toolchain/busybox-pocos.config)
for the exact applet list) over
[mlibc](https://github.com/managarm/mlibc).

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
   PIC, PIT timer, PS/2 keyboard and serial input, and the FPU/SSE state,
   then the physical/virtual memory managers and kernel heap. It mounts
   the writable FAT32 disk and loads `/busybox` (a real, dynamically-linked
   busybox binary -- see the top-level `Makefile`'s `busybox/busybox` rule)
   off it as the first process, invoked with `argv[0] = "sh"` so busybox's
   own applet dispatch runs its `ash` shell directly, before handing off to
   the scheduler. `ash`'s standalone-shell feature means every other
   configured applet (`ls`, `cat`, `mkdir`, ...) is also just this one
   binary -- there's no symlink support in `fat32.c` to install one file
   per applet name (see `fat32.h`'s doc comment), so `busybox` is the only
   binary on `disk.img`.

### Limine requests

Limine's boot protocol works by having the kernel place statically
declared "request" structs in a known linker section (`.requests`,
bracketed by `.requests_start_marker` / `.requests_end_marker` — see
[kernel/linker.ld](kernel/linker.ld)). Limine scans the kernel ELF for
these before jumping to `kmain` and fills in each request's `.response`
field. This kernel requests: the memory map, the HHDM (higher-half direct
map) offset, and boot modules (the initrd) — all declared in `main.c`.
[kernel/include/limine.h](kernel/include/limine.h) is the vendored protocol
header defining these structs/macros — it's copied verbatim from the
Limine project and intentionally left uncommented/unmodified here.

## Repository layout

```
lucy-os/
├── Makefile               top-level build: kernel + userland + mlibc + disk.img/initrd -> bootable ISO
├── limine.conf             Limine bootloader configuration
├── initrd/                 files bundled into initrd.tar (read-only, kernel-internal only)
├── kernel/
│   ├── Makefile            compiles kernel/src/*.c and *.S -> kernel/bin/kernel
│   ├── linker.ld           memory layout: higher-half link address, segments
│   ├── include/            every kernel header (.h) -- kept separate from src/'s
│   │                       .c/.S implementation files, one shared include dir
│   │   ├── limine.h            vendored Limine boot protocol header (do not edit)
│   │   └── ...                 one header per src/ module below, same name
│   └── src/
│       ├── main.c              kernel entry point (kmain): brings up every subsystem below
│       ├── serial.c            16550 UART driver (COM1): debug console + interrupt-driven input
│       ├── gdt.c               GDT + TSS (kernel/user selectors, IST double-fault stack)
│       ├── idt.c               IDT (256 gates)
│       ├── isr.c               CPU exception + IRQ dispatch, struct registers
│       ├── asm_stubs.S         gdt_flush/idt_flush/tss_flush, isr0-31, irq0-15, syscall_stub
│       ├── pic.c               8259 PIC remap/EOI/masking
│       ├── pit.c                PIT timer (IRQ0), tick counter, scheduler tick callback
│       ├── keyboard.c          PS/2 keyboard (IRQ1), scancode-to-ASCII
│       ├── console.c           tty line discipline (canonical/raw, echo, Ctrl-C, job control)
│       ├── fpu.c               x87/SSE enable at boot, per-process FXSAVE/FXRSTOR
│       ├── pmm.c               physical frame allocator (bitmap, built from Limine's memmap)
│       ├── vmm.c               per-address-space page table mapping (PML4 per process)
│       ├── heap.c              kmalloc/kfree (implicit free list over a VMM region)
│       ├── usercopy.c          validated copy_from_user()/copy_to_user() for syscall pointers
│       ├── elf.c               ELF64 loader (static + PIE, PT_INTERP, initial stack/auxv)
│       ├── process.c           preemptive round-robin scheduler: fork/execve/waitpid, fds,
│       │                       pipes, signals, anon mmap
│       ├── syscall.c           int 0x80 dispatch (PoC-OS's own syscall ABI)
│       ├── tarfs.c             read-only USTAR (tar) reader for the initrd
│       ├── virtio_blk.c        virtio-blk driver (legacy virtio-pci) -- the writable disk
│       ├── fat32.c             read-write FAT32 driver over virtio_blk.h
│       ├── vfs.c               cwd-relative path resolution over fat32.c
│       └── string.c            freestanding memcpy/memset/memmove/memcmp
├── userland/               linker.ld/linker-pie.ld shared by anything linked against mlibc
├── tools/                  mkfat32.py (disk image builder), setup_mlibc.py/gen_mlibc_stubs.py
├── mlibc/                  fetched by `make` on demand, not committed (managarm/mlibc checkout)
├── busybox/                fetched by `make` on demand, not committed (busybox.net release tarball)
├── toolchain/               cross GCC/binutils build + mlibc-sysdeps-pocos/ (our sysdeps port) +
│                            busybox-pocos.config/pocos-gcc.specs -- the parts of toolchain/ that
│                            ARE version-controlled
└── limine/                 fetched by `make` on demand, not committed
```

`kernel/bin/`, `kernel/obj/`, `hello-os.iso`, `disk.img`, `initrd.tar`,
`limine/`, `mlibc/`, `busybox/`, and most of `toolchain/` are all build
outputs / fetched dependencies — none of them are checked into version
control (see `.gitignore`); `make` regenerates them from source.

## Requirements

- `clang` and `ld.lld` (LLVM's linker) — used for cross-compiling
  freestanding, no-red-zone, `mcmodel=kernel` code without needing a
  separately built GCC cross-toolchain.
- `xorriso` — builds the hybrid BIOS/UEFI bootable ISO image.
- `tar` — builds the initrd (`--format=ustar`, which `kernel/src/tarfs.c`'s
  parser depends on).
- `git` — used by the top-level Makefile to fetch Limine and mlibc.
- `meson`/`ninja`, `python3` — used to build the mlibc port (see the
  Makefile's `mlibc-sysroot`/`mlibc-sysroot-shared` targets).
- A real cross GCC + binutils targeting `x86_64-elf` — built from source by
  the Makefile's `cross-gcc`/`toolchain/cross/bin/x86_64-elf-ld` targets
  (needs `gmp`/`mpfr`/`libmpc`/`zlib` on the host); used to build busybox,
  which expects a normal `$(CROSS_COMPILE)gcc` rather than clang's
  `-target` spelling.
- `qemu-system-x86_64` — to run the kernel in a virtual machine (not
  required just to build it).

On macOS: `brew install llvm xorriso qemu meson ninja`, then make sure the
Homebrew LLVM's `clang`/`ld.lld`/`llvm-ar`/`llvm-ranlib` are on `PATH` ahead
of Xcode's.

## Building and running

```sh
make        # build the kernel, mlibc userland, disk.img/initrd.tar, and hello-os.iso
make run    # build (if needed) and boot hello-os.iso in QEMU
```

`make run` passes `-serial stdio`, so kernel log lines (see `serial_print`
calls throughout `kernel/src/`) print in the same terminal as busybox's own
`ash` prompt. A working boot should log GDT/IDT/PIC/PIT/keyboard,
PMM/VMM/heap initialization, the FAT32 disk mounting, and then a
`BusyBox v1.36.1 ... built-in shell (ash)` banner followed by a `$` prompt
you can type into directly (`ls`, `mkdir foo`, `cd foo`, `echo hi > a.txt`,
`cat a.txt`, `mv`/`rm`/`rmdir`, `grep`, `find`, ... — see
`toolchain/busybox-pocos.config` for the full applet list).

### Cleaning

```sh
make clean       # remove kernel/bin, kernel/obj, hello-os.iso, disk.img, initrd.tar
make distclean   # clean, plus remove the fetched limine/, mlibc/, busybox/ directories
```

`make distclean` requires network access on the next build, since it
re-clones/re-downloads Limine, mlibc, and busybox.

## Extending this kernel

Some ideas for what's next, roughly in increasing order of effort:

- Real blocking I/O: `SYS_READ` never blocks today (see syscall.c's file
  header comment) since `int 0x80` is a DPL3 *interrupt* gate, which
  clears IF on entry — a real block/wake scheduler primitive would let
  userspace stop polling.
- VFAT long filenames (fat32.c only understands 8.3 short names today,
  which caps every path component busybox creates/renames at 8.3).
- A real TLB-shootdown-aware SMP scheduler (this one assumes a single
  CPU).
- Process groups / a real job-control model (console.c tracks a single
  foreground pid, not process groups) — `ash`'s own job control
  (`CONFIG_ASH_JOB_CONTROL`) is deliberately left off in
  `toolchain/busybox-pocos.config` for exactly this reason, so `bg`/`fg`/
  `jobs`/^Z don't work yet.
- More busybox applets: anything needing `mount`/`/proc`/networking/a
  passwd database won't work without real kernel support behind it (see
  `toolchain/busybox-pocos.config`'s own doc comment for what's
  deliberately left off and why).

Anywhere you add a new Limine request, declare it the same way
`module_request` is declared in `main.c` — `static volatile`, in the
`.requests` section — so Limine's scanner can find it before jumping to
`kmain`.
