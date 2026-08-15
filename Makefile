# The first rule in the file is normally make's default goal, but that's
# the build/-directory-creation rule below (needed as an order-only
# prerequisite elsewhere) rather than a real build target - pin the
# default explicitly so a bare `make` builds the OS image like `make all`.
.DEFAULT_GOAL := all

# ARCH selects the target: 64 (default) builds a native x86-64 long-mode
# kernel; 32 builds the original x86 protected-mode kernel. Most C sources
# are shared and branch on the X64 preprocessor macro (see CPPFLAGS below);
# a handful of files that differ in kind rather than in a few constants -
# entry, context switch, trap entry/return, the raw instruction-wrapper
# asm, and the linker script - have distinct 32-bit and 64-bit versions,
# selected below via ENTRYOBJ/SWTCHOBJ/TRAPASMOBJ/X86ASMOBJ/KERNELLD.
ARCH ?= 64

OBJS = \
	$(OBJDIR)/kernel/bio.o\
	$(OBJDIR)/kernel/console.o\
	$(OBJDIR)/kernel/exec.o\
	$(OBJDIR)/kernel/file.o\
	$(OBJDIR)/kernel/fs.o\
	$(OBJDIR)/kernel/ide.o\
	$(OBJDIR)/kernel/ioapic.o\
	$(OBJDIR)/kernel/kalloc.o\
	$(OBJDIR)/kernel/kbd.o\
	$(OBJDIR)/kernel/lapic.o\
	$(OBJDIR)/kernel/log.o\
	$(OBJDIR)/kernel/main.o\
	$(OBJDIR)/kernel/mp.o\
	$(OBJDIR)/kernel/picirq.o\
	$(OBJDIR)/kernel/pipe.o\
	$(OBJDIR)/kernel/proc.o\
	$(OBJDIR)/kernel/sleeplock.o\
	$(OBJDIR)/kernel/spinlock.o\
	$(OBJDIR)/kernel/string.o\
	$(OBJDIR)/kernel/syscall.o\
	$(OBJDIR)/kernel/sysfile.o\
	$(OBJDIR)/kernel/sysproc.o\
	$(OBJDIR)/kernel/trap.o\
	$(OBJDIR)/kernel/uart.o\
	$(OBJDIR)/kernel/vectors.o\
	$(OBJDIR)/kernel/vm.o\

ifeq ($(ARCH),64)
ENTRYOBJ = $(OBJDIR)/kernel/entry64.o
SWTCHOBJ = $(OBJDIR)/kernel/swtch64.o
TRAPASMOBJ = $(OBJDIR)/kernel/trapasm64.o
X86ASMOBJ = $(OBJDIR)/kernel/x86_64.o
KERNELLD = kernel/kernel64.ld
INITCODEOBJ = $(OBJDIR)/user/initcode64.o
else
ENTRYOBJ = $(OBJDIR)/kernel/entry.o
SWTCHOBJ = $(OBJDIR)/kernel/swtch.o
TRAPASMOBJ = $(OBJDIR)/kernel/trapasm.o
X86ASMOBJ = $(OBJDIR)/kernel/x86.o
KERNELLD = kernel/kernel.ld
INITCODEOBJ = $(OBJDIR)/user/initcode.o
endif
OBJS += $(SWTCHOBJ) $(TRAPASMOBJ) $(X86ASMOBJ)

# Toolchain: x86_64-elf-gcc/ld (Homebrew, /usr/local/bin) is multilib - it
# takes -m32 as well as -m64 (see BOOTCFLAGS/CFLAGS below) and its ld
# supports both the elf_i386 and elf_x86_64 emulations - so this one
# toolchain builds ARCH=32, ARCH=64, and the always-32-bit boot
# sector/AP trampoline alike. No separate i686-elf- toolchain needed.
TOOLPREFIX = x86_64-elf-
CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# qemu-system-x86_64's CPU is backwards compatible with 32-bit protected
# mode, so the same binary boots an ARCH=32 kernel as well as ARCH=64.
QEMU = qemu-system-x86_64

# NASM assembles all the .asm (Intel-syntax) sources. The shared C headers
# in include/ (constants, struct layouts) are still expanded into them with
# the C preprocessor before NASM ever sees the file - see the %.o: %.asm
# rules below. BOOTNASMFLAGS is for the always-32-bit boot sector/AP
# trampoline; NASMFLAGS follows ARCH and is used for every other
# hand-written .asm source.
NASM = nasm
BOOTNASMFLAGS = -f elf32 -g
ifeq ($(ARCH),64)
NASMFLAGS = -f elf64 -g
else
NASMFLAGS = -f elf32 -g
endif

# All headers live in include/, shared by boot/, kernel/, user/ and mkfs/.
# -DX64 lets shared headers (types.h, mmu.h, memlayout.h, x86.h, proc.h,
# elf.h) branch to their 64-bit struct/constant definitions.
CPPFLAGS = -Iinclude
ifeq ($(ARCH),64)
CPPFLAGS += -DX64
endif

CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -O2 -Wall -MD -ggdb -Werror -fno-omit-frame-pointer -Wno-error=array-bounds -Wno-error=infinite-recursion -Wno-error=unused-but-set-variable
ifeq ($(ARCH),64)
CFLAGS += -m64
else
CFLAGS += -m32
endif
# Nothing here ever sets CR0/CR4 to enable SSE (no build does FPU/SSE
# state management for kernel *or* user code) - but x86_64-elf-gcc's
# codegen baseline includes SSE2 in both -m64 and -m32 mode (its -m32
# multilib isn't the plain no-SSE i386 baseline a dedicated 32-bit-only
# cross-gcc would default to), so GCC will happily autovectorize an
# ordinary scalar loop into SSE instructions at -O2 with no other
# prompting. Executing one with SSE disabled raises #UD/#NM; this early
# in boot, with no IDT installed yet, that's an instant triple fault -
# so -mgeneral-regs-only rules out vector codegen entirely, for both
# ARCH values and for kernel and user code alike.
CFLAGS += -mgeneral-regs-only
# x86_64-elf-gcc supports -fno-stack-protector and -fno-pie/-no-pie in
# both -m32 and -m64 mode, so these are added unconditionally.
CFLAGS += -fno-stack-protector -fno-pie -no-pie

# boot/*.c and kernel/entryother.asm compile with the fixed-32-bit
# BOOTNASMFLAGS above, but still want the same warning/codegen posture as
# CFLAGS, so BOOTCFLAGS mirrors it rather than reusing CFLAGS directly
# (whose -m64/-m32 tracks ARCH, not "always 32-bit") - including
# -mgeneral-regs-only, for the same reason: boot code runs in
# real/protected mode with no SSE state management either.
BOOTCFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -O2 -Wall -MD -ggdb -m32 -Werror -fno-omit-frame-pointer -Wno-error=array-bounds -Wno-error=infinite-recursion -Wno-error=unused-but-set-variable -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie

# KCFLAGS adds flags needed only for kernel C code proper (not the boot
# sector, not user programs) on the 64-bit build: -mcmodel=kernel because
# KERNBASE (0xFFFFFFFF80000000, see memlayout.h) is in the top -2GB, which
# is exactly the address range GCC's "kernel" code model - as opposed to
# the default "small" model, which assumes symbols live in the *low* 2GB -
# is for; -mno-red-zone because an interrupt can land on the kernel stack
# at any instruction boundary and push a trap frame below the current
# %rsp, which would silently corrupt a leaf function's red zone.
ifeq ($(ARCH),64)
KCFLAGS = -mcmodel=kernel -mno-red-zone
else
KCFLAGS =
endif

# ld's emulation name for each target, hardcoded (confirmed via
# `x86_64-elf-ld -V` on this machine: elf_x86_64 / elf_i386, not the
# FreeBSD elf_i386_fbsd variant).
ifeq ($(ARCH),64)
LDFLAGS += -m elf_x86_64
else
LDFLAGS += -m elf_i386
endif
BOOTLDFLAGS += -m elf_i386

# Every generated file lives under build/: final binaries, disk images,
# and disassembly/symbol dumps directly in build/, and every intermediate
# .o/.d/.i file under build/obj/, mirroring the source tree
# (build/obj/kernel/, build/obj/user/, build/obj/boot/). Keeping the
# object tree at a distinct path from the final binaries matters, not
# just style: build/kernel (the linked kernel binary) and a hypothetical
# build/kernel/ (a directory of kernel object files) can't both exist, so
# intermediates get their own build/obj/ subtree instead of colliding
# with the very product names they build towards - the same reason
# kernel/, user/, and boot/ exist as directories one level up in the
# first place. boot/, kernel/, user/, include/, and mkfs/ contain only
# hand-written (or, for kernel/vectors.pl's output, generated-but-
# that's-the-point) source, never build output; `make clean` is just
# `rm -rf build`.
BUILD = build
OBJDIR = $(BUILD)/obj$(ARCH)

$(BUILD) $(OBJDIR)/boot $(OBJDIR)/kernel $(OBJDIR)/user $(OBJDIR)/musl-test:
	mkdir -p $@

# Compile a C source into build/obj<ARCH>/<dir>/<name>.o, keeping the same
# boot/kernel/user split the sources themselves use. boot/ always uses the
# fixed-32-bit BOOTCFLAGS; kernel/ adds KCFLAGS on top of the
# ARCH-selected CFLAGS.
$(OBJDIR)/boot/%.o: boot/%.c | $(OBJDIR)/boot
	$(CC) $(BOOTCFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR)/kernel/%.o: kernel/%.c | $(OBJDIR)/kernel
	$(CC) $(CFLAGS) $(KCFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR)/user/%.o: user/%.c | $(OBJDIR)/user
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Assemble a NASM source the same way: first expand #include/#define from
# include/ with the C preprocessor (cpp doesn't care about NASM vs GAS
# mnemonics, it just does text substitution), then hand the result to nasm.
$(OBJDIR)/boot/%.o: boot/%.asm | $(OBJDIR)/boot
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(BOOTNASMFLAGS) -o $@ $(@:.o=.i)

$(OBJDIR)/kernel/%.o: kernel/%.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(NASMFLAGS) -o $@ $(@:.o=.i)

# kernel/entryother.asm is the one kernel/*.asm source that (until it grows
# a 64-bit sibling in a later phase) always stays 16/32-bit real/protected
# mode, like boot/*.asm - this explicit rule overrides the generic
# ARCH-following pattern above for this file only.
$(OBJDIR)/kernel/entryother.o: kernel/entryother.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(BOOTNASMFLAGS) -o $@ $(@:.o=.i)

$(OBJDIR)/user/%.o: user/%.asm | $(OBJDIR)/user
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(NASMFLAGS) -o $@ $(@:.o=.i)

# poc.img layout: sector 0 = bootblock (stage 1, signed - see below),
# sectors 1..256 = boot2 (stage 2, zero-padded to exactly that many
# sectors - see boot/bootmain.c's own STAGE2_SECTORS and boot/
# boot2main.c's matching KERNEL_LBA), sector 257 onward = the kernel
# ELF image itself, exactly where it always was relative to stage 1
# before stage 2 existed, just shifted by stage 2's own footprint.
$(BUILD)/poc.img: $(BUILD)/bootblock $(BUILD)/boot2 $(BUILD)/kernel | $(BUILD)
	dd if=/dev/zero of=$(BUILD)/poc.img count=10000
	dd if=$(BUILD)/bootblock of=$(BUILD)/poc.img conv=notrunc
	dd if=$(BUILD)/boot2 of=$(BUILD)/poc.img seek=1 conv=notrunc
	dd if=$(BUILD)/kernel of=$(BUILD)/poc.img seek=257 conv=notrunc

$(BUILD)/pocmemfs.img: $(BUILD)/bootblock $(BUILD)/kernelmemfs | $(BUILD)
	dd if=/dev/zero of=$(BUILD)/pocmemfs.img count=10000
	dd if=$(BUILD)/bootblock of=$(BUILD)/pocmemfs.img conv=notrunc
	dd if=$(BUILD)/kernelmemfs of=$(BUILD)/pocmemfs.img seek=1 conv=notrunc

# bootmain.c needs its own rule rather than the generic $(OBJDIR)/boot/%.o
# pattern above: the boot sector has a hard 510-byte budget (512 minus
# the 2-byte 0x55AA signature), and -O2 (the default in $(BOOTCFLAGS)) alone
# generates code too large to fit, so this keeps the lighter -O the
# original boot Makefile always used here, along with -nostdinc since
# the boot loader is freestanding.
$(OBJDIR)/boot/bootmain.o: boot/bootmain.c | $(OBJDIR)/boot
	$(CC) $(BOOTCFLAGS) $(CPPFLAGS) -fno-pic -O -nostdinc -c -o $@ $<

$(BUILD)/bootblock: $(OBJDIR)/boot/bootasm.o $(OBJDIR)/boot/bootmain.o | $(BUILD)
	$(LD) $(BOOTLDFLAGS) -N -e start -Ttext 0x7C00 -o $(OBJDIR)/boot/bootblock.o $(OBJDIR)/boot/bootasm.o $(OBJDIR)/boot/bootmain.o
	$(OBJDUMP) -S $(OBJDIR)/boot/bootblock.o > $(BUILD)/bootblock.dis
	$(OBJCOPY) -S -O binary -j .text $(OBJDIR)/boot/bootblock.o $(BUILD)/bootblock
	./boot/sign.pl $(BUILD)/bootblock

# boot2main.c (stage 2): unlike bootmain.c (stage 1), no 510-byte
# budget - the generic $(OBJDIR)/boot/%.o pattern's normal -O2 is fine
# - but still freestanding (-nostdinc, -fno-pic) like every boot/ file.
$(OBJDIR)/boot/boot2main.o: boot/boot2main.c | $(OBJDIR)/boot
	$(CC) $(BOOTCFLAGS) $(CPPFLAGS) -fno-pic -nostdinc -c -o $@ $<

# BOOT2_MAX_SECTORS must match boot/bootmain.c's own STAGE2_SECTORS -
# how many sectors stage 1 reads stage 2 into before jumping to it.
# Linked to run at 0x10000 (boot/bootmain.c's STAGE2_ADDR) with -N (no
# separate page-aligned segments - same reason bootblock itself uses
# it: this raw-binary-extracted image can only have one contiguous
# blob, not a gap-separated set of segments) and -e entry2 - boot/
# boot2asm.o's own tiny stub, linked *first* (see that file's own
# comment for why -e alone, or source-order alone, isn't enough once
# this gets flattened to a raw binary and jumped to by hardcoded
# address).
BOOT2_MAX_SECTORS = 256
$(BUILD)/boot2: $(OBJDIR)/boot/boot2asm.o $(OBJDIR)/boot/boot2main.o | $(BUILD)
	$(LD) $(BOOTLDFLAGS) -N -e entry2 -Ttext 0x10000 -o $(OBJDIR)/boot/boot2.o $(OBJDIR)/boot/boot2asm.o $(OBJDIR)/boot/boot2main.o
	$(OBJDUMP) -S $(OBJDIR)/boot/boot2.o > $(BUILD)/boot2.dis
	$(OBJCOPY) -S -O binary -j .text -j .rodata -j .data $(OBJDIR)/boot/boot2.o $(BUILD)/boot2
	size=$$(stat -f%z $(BUILD)/boot2 2>/dev/null || stat -c%s $(BUILD)/boot2); \
	max=$$(( $(BOOT2_MAX_SECTORS) * 512 )); \
	if [ "$$size" -gt "$$max" ]; then \
		echo "boot2 too large: $$size bytes (max $$max, $(BOOT2_MAX_SECTORS) sectors)" >&2; \
		exit 1; \
	fi

# ============================================================
# BIOS/INT13h boot path: real hardware (legacy IDE, or SATA in
# AHCI mode via a real BIOS/CSM AHCI driver, or booted from USB),
# VirtualBox, and QEMU all boot through BIOS/CSM firmware's own
# INT13h disk services - unlike the ATA-PIO path above (poc.img,
# fs.img as a *separate* drive), which only works when the disk is
# attached as an emulated/real legacy IDE hard disk. See boot/
# bootasm_bios.asm's and boot/boot2_bios.asm's own comments for the
# full reasoning. Everything here - bootloader, kernel, and the
# whole root filesystem image - lives on one combined disk image
# (poc_bios.img), since a real USB stick or a real internal disk is
# one physical device, not two.
# ============================================================

# kernel.bin: a raw, already-relocated flattening of the kernel ELF
# (same idea as bootblock/boot2/entryother/initcode - objcopy -O
# binary elsewhere in this Makefile - just applied to the kernel
# itself here) so boot/boot2_bios.asm never needs to parse an ELF
# header/program headers in real-mode assembly: the segments are
# already contiguous (kernel64.ld places .text at EXTMEM with
# .rodata/.data/.bss immediately following), so "load N bytes
# starting at physical EXTMEM" is the entire job.
# -O binary only ever contains *file-backed* content (PT_LOAD's
# p_filesz, not p_memsz) - it silently drops .bss (p_memsz > p_filesz:
# kernel/entry64.asm's own boot-time page tables, among other kernel
# globals, live there, reserved but zero-initialized rather than
# taking up file space) entirely. boot/boot2_bios.asm has no ELF
# parser to notice this and zero the gap itself the way boot/
# boot2main.c's real per-segment stosb() loop does (see its own
# comment for why this port isn't writing one in real-mode assembly) -
# so it has to already be zeroed *in the file*, by padding kernel.bin
# out to the highest PT_LOAD segment's real (PhysAddr+MemSiz) extent,
# here, once, at build time.
$(BUILD)/kernel.bin: $(BUILD)/kernel | $(BUILD)
	$(OBJCOPY) -S -O binary $(BUILD)/kernel $(BUILD)/kernel.bin
	total=$$($(TOOLPREFIX)readelf -W -l $(BUILD)/kernel | python3 -c '\
import sys; lines=[l.split() for l in sys.stdin if l.strip().startswith("LOAD")]; \
paddrs=[int(f[3],16) for f in lines]; ends=[int(f[3],16)+int(f[5],16) for f in lines]; \
print(max(ends)-min(paddrs))'); \
	truncate -s $$total $(BUILD)/kernel.bin 2>/dev/null || dd if=/dev/zero bs=1 count=0 seek=$$total of=$(BUILD)/kernel.bin conv=notrunc 2>/dev/null

# bootconfig_bios.h: KERNEL_SECTORS/FS_IMG_LBA/KERNEL_ENTRY are real
# properties of a specific build (the kernel's actual size and entry
# point), not constants boot2_bios.asm should hardcode by hand -
# generated here the same way MUSL_GENH's headers are, and %included
# by boot2_bios.asm via the usual cpp-then-nasm pipeline (see its own
# build rule below for the extra -I this needs).
# BOOT2_BIOS_MAX_SECTORS/BOOT2_BIOS_LBA must match boot/bootasm_bios.asm's
# own STAGE2_SECTORS - how many sectors stage 1 reads stage 2 into
# (starting at LBA 1) before jumping to it. Far smaller than the ATA-
# PIO path's BOOT2_MAX_SECTORS: this stage 2 is hand-written assembly
# with no ELF-parsing logic at all (see boot/boot2_bios.asm's own
# comment for why), so it doesn't need nearly as much room.
BOOT2_BIOS_MAX_SECTORS = 16
BOOT2_BIOS_LBA = $(shell echo $$((1 + $(BOOT2_BIOS_MAX_SECTORS))))

$(OBJDIR)/boot/bootconfig_bios.h: $(BUILD)/kernel.bin $(BUILD)/kernel | $(OBJDIR)/boot
	kbytes=$$(stat -f%z $(BUILD)/kernel.bin 2>/dev/null || stat -c%s $(BUILD)/kernel.bin); \
	ksectors=$$(( (kbytes + 511) / 512 )); \
	kentry=$$($(OBJDUMP) -f $(BUILD)/kernel | sed -n 's/^start address //p'); \
	{ \
		echo "#define KERNEL_LBA $(BOOT2_BIOS_LBA)"; \
		echo "#define KERNEL_SECTORS $$ksectors"; \
		echo "#define FS_IMG_LBA (KERNEL_LBA + KERNEL_SECTORS)"; \
		echo "#define KERNEL_ENTRY $$kentry"; \
	} > $(OBJDIR)/boot/bootconfig_bios.h

$(BUILD)/bootblock_bios: $(OBJDIR)/boot/bootasm_bios.o | $(BUILD)
	$(LD) $(BOOTLDFLAGS) -N -e start -Ttext 0x7C00 -o $(OBJDIR)/boot/bootblock_bios.o $(OBJDIR)/boot/bootasm_bios.o
	$(OBJDUMP) -S $(OBJDIR)/boot/bootblock_bios.o > $(BUILD)/bootblock_bios.dis
	$(OBJCOPY) -S -O binary -j .text $(OBJDIR)/boot/bootblock_bios.o $(BUILD)/bootblock_bios
	./boot/sign.pl $(BUILD)/bootblock_bios

# boot2_bios.asm needs bootconfig_bios.h visible on its own include
# path - the one file in boot/ that does, hence its own rule rather
# than the generic $(OBJDIR)/boot/%.o: boot/%.asm pattern.
$(OBJDIR)/boot/boot2_bios.o: boot/boot2_bios.asm $(OBJDIR)/boot/bootconfig_bios.h | $(OBJDIR)/boot
	$(CC) $(CPPFLAGS) -I$(OBJDIR)/boot -E -x assembler-with-cpp -o $(OBJDIR)/boot/boot2_bios.i boot/boot2_bios.asm
	$(NASM) $(BOOTNASMFLAGS) -o $@ $(OBJDIR)/boot/boot2_bios.i

# -Ttext 0x1000, not 0x10000 like the ATA-PIO path's boot2 - see boot/
# bootasm_bios.asm's own comment on STAGE2_ADDR for why (16-bit ELF
# relocations can't represent an address >= 0x10000).
$(BUILD)/boot2_bios: $(OBJDIR)/boot/boot2_bios.o | $(BUILD)
	$(LD) $(BOOTLDFLAGS) -N -e entry2 -Ttext 0x1000 -o $(OBJDIR)/boot/boot2_bios_full.o $(OBJDIR)/boot/boot2_bios.o
	$(OBJDUMP) -S $(OBJDIR)/boot/boot2_bios_full.o > $(BUILD)/boot2_bios.dis
	$(OBJCOPY) -S -O binary -j .text -j .rodata -j .data $(OBJDIR)/boot/boot2_bios_full.o $(BUILD)/boot2_bios
	size=$$(stat -f%z $(BUILD)/boot2_bios 2>/dev/null || stat -c%s $(BUILD)/boot2_bios); \
	max=$$(( $(BOOT2_BIOS_MAX_SECTORS) * 512 )); \
	if [ "$$size" -gt "$$max" ]; then \
		echo "boot2_bios too large: $$size bytes (max $$max, $(BOOT2_BIOS_MAX_SECTORS) sectors)" >&2; \
		exit 1; \
	fi

# poc_bios.img layout: sector 0 = bootblock_bios (stage 1), sectors
# 1..BOOT2_BIOS_MAX_SECTORS = boot2_bios (stage 2), sector KERNEL_LBA
# (BOOT2_BIOS_LBA, bootconfig_bios.h) onward = kernel.bin, then
# immediately following (FS_IMG_LBA = KERNEL_LBA + KERNEL_SECTORS) =
# fs.img - one combined disk image, unlike poc.img+fs.img's two
# separate drives, since real boot media (a USB stick, an internal
# disk) is one physical device.
$(BUILD)/poc_bios.img: $(BUILD)/bootblock_bios $(BUILD)/boot2_bios $(BUILD)/kernel.bin $(BUILD)/fs.img $(OBJDIR)/boot/bootconfig_bios.h | $(BUILD)
	dd if=/dev/zero of=$(BUILD)/poc_bios.img count=15000
	dd if=$(BUILD)/bootblock_bios of=$(BUILD)/poc_bios.img conv=notrunc
	dd if=$(BUILD)/boot2_bios of=$(BUILD)/poc_bios.img seek=1 conv=notrunc
	dd if=$(BUILD)/kernel.bin of=$(BUILD)/poc_bios.img seek=$(BOOT2_BIOS_LBA) conv=notrunc
	fsimglba=$$(sed -n 's/^#define KERNEL_SECTORS //p' $(OBJDIR)/boot/bootconfig_bios.h | awk '{print $$1+$(BOOT2_BIOS_LBA)}'); \
	dd if=$(BUILD)/fs.img of=$(BUILD)/poc_bios.img seek=$$fsimglba conv=notrunc

# One ISO, bootable identically on real hardware (Legacy/CSM BIOS off
# a USB stick), VirtualBox, and QEMU, whether attached as an optical
# drive or dd'd straight to a USB stick (isohybrid-style: it's both a
# valid ISO9660 filesystem *and* the same raw, MBR-bootable disk image
# poc_bios.img already is - see boot/bootasm_bios.asm's own comment
# for why that image reads via CHS, not LBA extensions: found the hard
# way that El-Torito "hard disk emulation" CD-boot - the mechanism
# that lets a plain BIOS bootloader address an El-Torito-mounted ISO
# at all - fails INT13h-extensions-present outright on real BIOS/
# QEMU/SeaBIOS alike, while CHS is the one interface universal across
# real disks, USB, and El-Torito emulation). -hard-disk-boot tells
# xorriso to register the whole image as a "hard disk emulation" El
# Torito boot entry (not "no emulation", which handed back the CD's
# native drive - passed the extensions check but then hung on the
# actual extended read; a QEMU/SeaBIOS ATAPI-sector-size quirk, near
# as can be told) - boot-load-size is irrelevant in that mode (BIOS
# always loads exactly the one MBR sector, like any real hard disk
# boot) but xorriso still wants a value.
$(BUILD)/poc-os.iso: $(BUILD)/poc_bios.img | $(BUILD)
	rm -rf $(BUILD)/isoroot
	mkdir -p $(BUILD)/isoroot
	cp $(BUILD)/poc_bios.img $(BUILD)/isoroot/boot.img
	xorriso -as mkisofs -o $(BUILD)/poc-os.iso -V POCOS \
		-b boot.img -hard-disk-boot -boot-load-size 1 $(BUILD)/isoroot

# entryother and initcode are raw binary blobs the kernel embeds with
# -b binary (see kernel/main.c's and kernel/proc.c's matching
# _binary_build_..._start symbols) rather than programs run standalone,
# so unlike everything else in $(OBJS)/ULIB they need their own two-step
# ld+objcopy recipe instead of just landing in a link line. They're
# final build products, not intermediates, so - like bootblock, kernel,
# and mkfs - they live directly in build/, not build/obj<ARCH>/. entryother
# is always the fixed-32-bit trampoline, linked with BOOTLDFLAGS
# (see entryother.o's rule above); initcode is $(INITCODEOBJ), which
# already matches ARCH, so it links with the ARCH-selected LDFLAGS like
# everything else in $(OBJS).
$(BUILD)/entryother: $(OBJDIR)/kernel/entryother.o | $(BUILD)
	$(LD) $(BOOTLDFLAGS) -N -e start -Ttext 0x7000 -o $(OBJDIR)/kernel/bootblockother.o $(OBJDIR)/kernel/entryother.o
	$(OBJCOPY) -S -O binary -j .text $(OBJDIR)/kernel/bootblockother.o $(BUILD)/entryother
	$(OBJDUMP) -S $(OBJDIR)/kernel/bootblockother.o > $(BUILD)/entryother.dis

$(BUILD)/initcode: $(INITCODEOBJ) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $(OBJDIR)/user/initcode.out $(INITCODEOBJ)
	$(OBJCOPY) -S -O binary $(OBJDIR)/user/initcode.out $(BUILD)/initcode
	$(OBJDUMP) -S $(INITCODEOBJ) > $(BUILD)/initcode.dis

$(BUILD)/kernel: $(OBJS) $(ENTRYOBJ) $(BUILD)/entryother $(BUILD)/initcode $(KERNELLD) | $(BUILD)
	$(LD) $(LDFLAGS) -T $(KERNELLD) -o $(BUILD)/kernel $(ENTRYOBJ) $(OBJS) -b binary $(BUILD)/initcode $(BUILD)/entryother
	$(OBJDUMP) -S $(BUILD)/kernel > $(BUILD)/kernel.dis
	$(OBJDUMP) -t $(BUILD)/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/kernel.sym

# kernelmemfs is a copy of kernel that maintains the
# disk image in memory instead of writing to a disk.
# This is not so useful for testing persistent storage or
# exploring disk buffering implementations, but it is
# great for testing the kernel on real hardware without
# needing a scratch disk.
MEMFSOBJS = $(filter-out $(OBJDIR)/kernel/ide.o,$(OBJS)) $(OBJDIR)/kernel/memide.o
$(BUILD)/kernelmemfs: $(MEMFSOBJS) $(ENTRYOBJ) $(BUILD)/entryother $(BUILD)/initcode $(KERNELLD) $(BUILD)/fs.img | $(BUILD)
	$(LD) $(LDFLAGS) -T $(KERNELLD) -o $(BUILD)/kernelmemfs $(ENTRYOBJ) $(MEMFSOBJS) -b binary $(BUILD)/initcode $(BUILD)/entryother $(BUILD)/fs.img
	$(OBJDUMP) -S $(BUILD)/kernelmemfs > $(BUILD)/kernelmemfs.dis
	$(OBJDUMP) -t $(BUILD)/kernelmemfs | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/kernelmemfs.sym

tags:
	etags boot/*.asm boot/*.c kernel/*.asm kernel/*.c user/*.c user/*.asm include/*.h mkfs/*.c

# Generated (not hand-written), so it lives under build/ like every other
# build product, even though NASM will treat it as a source file. Needs
# its own rule rather than the generic $(OBJDIR)/kernel/%.o: kernel/%.asm
# pattern above, since vectors.asm's "source" is itself a build product,
# not a file that exists under kernel/.
$(OBJDIR)/kernel/vectors.asm: kernel/vectors.pl | $(OBJDIR)/kernel
	./kernel/vectors.pl $(ARCH) > $(OBJDIR)/kernel/vectors.asm

$(OBJDIR)/kernel/vectors.o: $(OBJDIR)/kernel/vectors.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(OBJDIR)/kernel/vectors.i $(OBJDIR)/kernel/vectors.asm
	$(NASM) $(NASMFLAGS) -o $@ $(OBJDIR)/kernel/vectors.i

USYSOBJ = $(if $(filter 64,$(ARCH)),$(OBJDIR)/user/usys64.o,$(OBJDIR)/user/usys.o)
ULIB = $(OBJDIR)/user/ulib.o $(USYSOBJ) $(OBJDIR)/user/printf.o $(OBJDIR)/user/umalloc.o $(X86ASMOBJ)

# Debug info (-ggdb, in CFLAGS) is generated for every user binary, same
# as the kernel, but it's dead weight once mkfs packages the binary into
# the filesystem image: nothing in poc reads DWARF at runtime, and the
# generic $(OBJDUMP) -S/-t steps just above already captured everything
# it's useful for (source-interleaved disassembly, a symbol table) into
# build/$*.dis/.sym, which stay on the host, not in the image. Stripping
# it here (after those, so they still see it) routinely more than
# halves the linked binary's size - the difference between fitting
# under MAXFILE (include/fs.h, ~70KB - see the UPROGS comment) and not,
# for a binary like usertests with enough test code to approach it.
$(BUILD)/_%: $(OBJDIR)/user/%.o $(ULIB) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/$*.dis
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/$*.sym
	$(OBJCOPY) --strip-debug $@

# musl-test/: home to runmusl.c (a manual launcher for a musl-crt1-style
# binary via raw SYS_execve - "runmusl <path> [args...]"; user/sh.c's
# own isdyn() check in runcmd() makes this unnecessary for the ordinary
# case of just running true/false/cat by name, but it's still useful
# standalone, e.g. to pass a path sh.c can't parse as a bare command)
# and ldso_stubs.c (linked into libc.so itself - see MUSL_LDSO_OBJS).
# Unlike ordinary user/ programs these need neither ULIB nor an -e main convention shared
# with xv6's own ulib.c (they don't link ulib.c at all). -Iinclude is
# here too (not just -Imusl/arch/x86_64) since these reference poc-os
# kernel constants directly (e.g. ARCH_SET_FS from include/syscall.h)
# rather than only musl's own headers.
$(OBJDIR)/musl-test/%.o: musl/test/%.c | $(OBJDIR)/musl-test
	$(CC) $(CFLAGS) -Imusl/arch/x86_64 -Iinclude -c -o $@ $<

# musl/: real musl source (musl-test/ above is only raw syscall_arch.h
# smoke tests; this is actual crt1/__libc_start_main/malloc/etc object
# code). musl's own build system generates two headers from
# arch/x86_64/bits/*.in - obj/include/bits/alltypes.h and bits/
# syscall.h - normally by running its ./configure && make; the two
# recipes below just replicate those two exact commands (see musl/
# Makefile's own obj/include/bits/%.h rules) without depending on
# musl's build system or a prior ./configure, since all we want out of
# it is these two files.
$(OBJDIR)/musl/include/bits/alltypes.h: musl/arch/x86_64/bits/alltypes.h.in musl/include/alltypes.h.in musl/tools/mkalltypes.sed
	@mkdir -p $(dir $@)
	sed -f musl/tools/mkalltypes.sed musl/arch/x86_64/bits/alltypes.h.in musl/include/alltypes.h.in > $@

$(OBJDIR)/musl/include/bits/syscall.h: musl/arch/x86_64/bits/syscall.h.in
	@mkdir -p $(dir $@)
	cp $< $@
	sed -n -e s/__NR_/SYS_/p < $< >> $@

MUSL_GENH = $(OBJDIR)/musl/include/bits/alltypes.h $(OBJDIR)/musl/include/bits/syscall.h

# Mirrors musl's own CFLAGS_ALL/IFLAGS (see musl/Makefile) so its
# source compiles unmodified: -nostdinc/-ffreestanding because it's
# its own libc, not something built against poc-os's (or the host's)
# headers; the -I order matters (arch-specific bits before generic,
# musl's own src/internal/syscall.h etc before its public include/,
# $(OBJDIR)/musl/include last among musl's own so a real header always
# wins over the generated stand-in) since a couple of names exist at
# more than one of these paths on purpose (bits/syscall.h.in and its
# generated bits/syscall.h in particular).
MUSL_CFLAGS = -std=c99 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -Os \
	-m64 -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie \
	-fno-omit-frame-pointer -g -Wall
MUSL_INC = -Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/internal \
	-Imusl/src/include -Imusl/src/internal -I$(OBJDIR)/musl/include -Imusl/include

# One rule for musl/**.c, however deep - unlike boot/kernel/user's
# fixed one-level split, musl's source tree has real subdirectories
# (src/env/, src/thread/x86_64/, crt/, ...), so this mirrors each
# source's path under $(OBJDIR)/musl/ via $(dir $@) rather than
# pre-declaring every subdirectory as an order-only prerequisite the
# way the rest of this Makefile does.
$(OBJDIR)/musl/%.o: musl/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) $(MUSL_INC) -c -o $@ $<

# Ditto for musl's hand-written .s (AT&T syntax, assembled directly by
# $(CC) via binutils as - not one of poc-os's own NASM .asm sources).
$(OBJDIR)/musl/%.o: musl/%.s
	@mkdir -p $(dir $@)
	$(CC) -m64 -c -o $@ $<

# runmusl: generic launcher, "runmusl <path> [args...]" - see
# musl/test/runmusl.c. Entered the normal poc-os way (-e main, native
# SYS_exec) since it's poc-os code, not musl code; what it launches
# (SYS_execve on argv[1]) is what needs the musl-style stack.
$(BUILD)/_runmusl: $(OBJDIR)/musl-test/runmusl.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/runmusl.dis
	$(OBJCOPY) --strip-debug $@

# musl's own version.h (musl/src/internal/version.c's #include
# "version.h"): normally generated by ./configure from musl/VERSION;
# this is the same one-line `#define VERSION "..."` musl's own
# Makefile produces, without needing to run ./configure at all - same
# rationale as MUSL_GENH's alltypes.h/syscall.h above.
$(OBJDIR)/musl/internal/version.h: musl/VERSION
	@mkdir -p $(dir $@)
	printf '#define VERSION "%s"\n' "$$(cat musl/VERSION)" > $@

# musl/ldso/ (real ld-musl-x86_64.so.1 - dlstart.c/dynlink.c, unmodified
# upstream - plus enough of real musl's own libc, also unmodified
# upstream, to link a genuine libc.so around them) and true/false/cat
# themselves (real dynamically-linked PIE ELF binaries importing libc
# via a real PT_INTERP/PLT/GOT, not musl-crt1-launched-by-runmusl like
# poc-os's own static binaries) live in their own $(OBJDIR)/musl-pic/
# tree, compiled -fPIC - shared-object code, unlike every other musl
# object in this Makefile (MUSL_CFLAGS above is -fno-pie/-no-pie, for
# poc-os's own always-static-at-0 binaries). A separate object tree
# rather than reusing $(OBJDIR)/musl/ so a source file needed by both
# a static MUSL_LIBC_OBJS-style binary (e.g. musl/src/string/memcpy.c)
# and libc.so doesn't fight over one non-PIC-vs-PIC .o.
MUSL_PIC_CFLAGS = $(MUSL_CFLAGS:-fno-pie=-fPIC)
MUSL_PIC_INC = -Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/internal \
	-Imusl/src/include -Imusl/src/internal -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/musl-pic/%.o: musl/%.c $(MUSL_GENH) $(OBJDIR)/musl/internal/version.h
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_PIC_CFLAGS) $(MUSL_PIC_INC) -c -o $@ $<

$(OBJDIR)/musl-pic/%.o: musl/%.s
	@mkdir -p $(dir $@)
	$(CC) -m64 -fPIC -c -o $@ $<

# coreutils_shims.c (see its own comment) is poc-os C, not gnulib or
# musl - but it fills in real libc-shaped gaps (a translating fstat(),
# mbszero(), abort(), ...), so it belongs in libc.so itself, available
# to every dynamically-linked binary, not statically re-linked into
# each one individually. Compiled exactly like any other musl-pic
# source (MUSL_PIC_CFLAGS/INC - see musl/src/internal/stdio_impl.h's
# role there, spelled out in the file's own comment), just from a
# source path outside musl/ - hence its own rule rather than the
# generic $(OBJDIR)/musl-pic/%.o: musl/%.c pattern above.
$(OBJDIR)/musl-pic/coreutils-shims/coreutils_shims.o: coreutils/poc/coreutils_shims.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_PIC_CFLAGS) $(MUSL_PIC_INC) -c -o $@ $<

# bash_shims.c (see its own comment): the same idea as coreutils_shims.c
# just above (real libc-shaped gaps - dup2(), uname(), getrlimit()/
# setrlimit(), times() - poc-os has no syscall for at all), also part of
# libc.so rather than linked per-executable, so any future dynamically-
# linked program needing dup2()/uname()/etc gets it for free too.
$(OBJDIR)/musl-pic/bash-shims/bash_shims.o: bash/poc/bash_shims.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_PIC_CFLAGS) $(MUSL_PIC_INC) -c -o $@ $<

# The real musl objects libc.so needs beyond MUSL_LIBC_OBJS/
# MUSL_MALLOC_OBJS/MUSL_STDIO_OBJS's already-proven subset - grown the
# same file-by-file way those were (see this Makefile's own git log),
# just starting from "does musl/ldso/dynlink.c link" instead of "does
# a musl-crt1 hello world link". crt1.o is deliberately excluded (see
# musl/crt/Scrt1.c below): it defines _start for a *static* program,
# not something libc.so itself needs.
MUSL_LDSO_OBJS = \
	$(OBJDIR)/musl-pic/src/env/__libc_start_main.o \
	$(OBJDIR)/musl-pic/src/env/__init_tls.o \
	$(OBJDIR)/musl-pic/src/env/__environ.o \
	$(OBJDIR)/musl-pic/src/internal/libc.o \
	$(OBJDIR)/musl-pic/src/internal/defsysinfo.o \
	$(OBJDIR)/musl-pic/src/internal/syscall_ret.o \
	$(OBJDIR)/musl-pic/src/thread/__syscall_cp.o \
	$(OBJDIR)/musl-pic/src/thread/default_attr.o \
	$(OBJDIR)/musl-pic/src/thread/x86_64/__set_thread_area.o \
	$(OBJDIR)/musl-pic/src/string/memcpy.o \
	$(OBJDIR)/musl-pic/src/errno/__errno_location.o \
	$(OBJDIR)/musl-pic/src/unistd/write.o \
	$(OBJDIR)/musl-pic/src/exit/exit.o \
	$(OBJDIR)/musl-pic/src/exit/_Exit.o \
	$(OBJDIR)/musl-pic/src/malloc/oldmalloc/malloc.o \
	$(OBJDIR)/musl-pic/src/malloc/lite_malloc.o \
	$(OBJDIR)/musl-pic/src/malloc/calloc.o \
	$(OBJDIR)/musl-pic/src/malloc/realloc.o \
	$(OBJDIR)/musl-pic/src/malloc/free.o \
	$(OBJDIR)/musl-pic/src/malloc/replaced.o \
	$(OBJDIR)/musl-pic/src/mman/mmap.o \
	$(OBJDIR)/musl-pic/src/mman/munmap.o \
	$(OBJDIR)/musl-pic/src/mman/madvise.o \
	$(OBJDIR)/musl-pic/src/mman/mremap.o \
	$(OBJDIR)/musl-pic/src/mman/mprotect.o \
	$(OBJDIR)/musl-pic/src/string/memset.o \
	$(OBJDIR)/musl-pic/src/string/strlen.o \
	$(OBJDIR)/musl-pic/src/thread/__wait.o \
	$(OBJDIR)/musl-pic/src/thread/__lock.o \
	$(OBJDIR)/musl-pic/src/stdio/vfprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/printf.o \
	$(OBJDIR)/musl-pic/src/stdio/vprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/__towrite.o \
	$(OBJDIR)/musl-pic/src/stdio/__overflow.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_write.o \
	$(OBJDIR)/musl-pic/src/stdio/stdout.o \
	$(OBJDIR)/musl-pic/src/stdio/__lockfile.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_exit.o \
	$(OBJDIR)/musl-pic/src/stdio/ofl.o \
	$(OBJDIR)/musl-pic/src/stdio/ofl_add.o \
	$(OBJDIR)/musl-pic/src/stdio/fwrite.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_close.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdout_write.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_seek.o \
	$(OBJDIR)/musl-pic/src/stdio/snprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/vsnprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/dprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/vdprintf.o \
	$(OBJDIR)/musl-pic/src/errno/strerror.o \
	$(OBJDIR)/musl-pic/src/string/strnlen.o \
	$(OBJDIR)/musl-pic/src/string/memchr.o \
	$(OBJDIR)/musl-pic/src/string/strcpy.o \
	$(OBJDIR)/musl-pic/src/string/strrchr.o \
	$(OBJDIR)/musl-pic/src/string/strchr.o \
	$(OBJDIR)/musl-pic/src/string/strchrnul.o \
	$(OBJDIR)/musl-pic/src/string/stpcpy.o \
	$(OBJDIR)/musl-pic/src/string/memrchr.o \
	$(OBJDIR)/musl-pic/src/string/strcspn.o \
	$(OBJDIR)/musl-pic/src/string/strspn.o \
	$(OBJDIR)/musl-pic/src/string/strncmp.o \
	$(OBJDIR)/musl-pic/src/string/memcmp.o \
	$(OBJDIR)/musl-pic/src/multibyte/wctomb.o \
	$(OBJDIR)/musl-pic/src/multibyte/wcrtomb.o \
	$(OBJDIR)/musl-pic/src/locale/__lctrans.o \
	$(OBJDIR)/musl-pic/src/unistd/lseek.o \
	$(OBJDIR)/musl-pic/src/unistd/pread.o \
	$(OBJDIR)/musl-pic/src/unistd/_exit.o \
	$(OBJDIR)/musl-pic/src/fcntl/open.o \
	$(OBJDIR)/musl-pic/src/unistd/close.o \
	$(OBJDIR)/musl-pic/src/unistd/read.o \
	$(OBJDIR)/musl-pic/src/env/getenv.o \
	$(OBJDIR)/musl-pic/src/malloc/libc_calloc.o \
	$(OBJDIR)/musl-pic/src/ldso/dlerror.o \
	$(OBJDIR)/musl-pic/src/ldso/tlsdesc.o \
	$(OBJDIR)/musl-pic/src/internal/version.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_mutex_lock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_mutex_unlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_cond_wait.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_cond_broadcast.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_rdlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_wrlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_unlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_tryrdlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_trywrlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_timedrdlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_timedwrlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_setcancelstate.o \
	$(OBJDIR)/musl-pic/src/thread/__tls_get_addr.o \
	$(OBJDIR)/musl-pic/src/thread/vmlock.o \
	$(OBJDIR)/musl-pic/src/setjmp/x86_64/setjmp.o \
	$(OBJDIR)/musl-pic/src/setjmp/x86_64/longjmp.o \
	$(OBJDIR)/musl-pic/ldso/dlstart.o \
	$(OBJDIR)/musl-pic/ldso/dynlink.o \
	$(OBJDIR)/musl-pic/test/ldso_stubs.o \
	$(OBJDIR)/musl-pic/src/ctype/__ctype_get_mb_cur_max.o \
	$(OBJDIR)/musl-pic/src/time/__map_file.o \
	$(OBJDIR)/musl-pic/src/locale/__mo_lookup.o \
	$(OBJDIR)/musl-pic/src/exit/atexit.o \
	$(OBJDIR)/musl-pic/src/locale/c_locale.o \
	$(OBJDIR)/musl-pic/src/stdio/fclose.o \
	$(OBJDIR)/musl-pic/src/fcntl/fcntl.o \
	$(OBJDIR)/musl-pic/src/stdio/ferror.o \
	$(OBJDIR)/musl-pic/src/stdio/fflush.o \
	$(OBJDIR)/musl-pic/src/stdio/fprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/fputs.o \
	$(OBJDIR)/musl-pic/src/stdio/putc.o \
	$(OBJDIR)/musl-pic/src/stdio/stderr.o \
	$(OBJDIR)/musl-pic/src/stdio/clearerr.o \
	$(OBJDIR)/musl-pic/src/stdio/fileno.o \
	$(OBJDIR)/musl-pic/src/multibyte/internal.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbrtoc32.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbrtowc.o \
	$(OBJDIR)/musl-pic/src/locale/langinfo.o \
	$(OBJDIR)/musl-pic/src/locale/locale_map.o \
	$(OBJDIR)/musl-pic/src/locale/setlocale.o \
	$(OBJDIR)/musl-pic/src/malloc/reallocarray.o \
	$(OBJDIR)/musl-pic/src/malloc/posix_memalign.o \
	$(OBJDIR)/musl-pic/src/malloc/oldmalloc/aligned_alloc.o \
	$(OBJDIR)/musl-pic/src/string/strcmp.o \
	$(OBJDIR)/musl-pic/src/string/strerror_r.o \
	$(OBJDIR)/musl-pic/src/string/memmove.o \
	$(OBJDIR)/musl-pic/src/misc/ioctl.o \
	$(OBJDIR)/musl-pic/src/legacy/getpagesize.o \
	$(OBJDIR)/musl-pic/src/unistd/unlink.o \
	$(OBJDIR)/musl-pic/src/unistd/link.o \
	$(OBJDIR)/musl-pic/src/stat/mkdir.o \
	$(OBJDIR)/musl-pic/src/stat/mknod.o \
	$(OBJDIR)/musl-pic/src/unistd/getpid.o \
	$(OBJDIR)/musl-pic/src/signal/kill.o \
	$(OBJDIR)/musl-pic/src/exit/assert.o \
	$(OBJDIR)/musl-pic/src/misc/getopt.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbtowc.o \
	$(OBJDIR)/musl-pic/src/stdio/putchar.o \
	$(OBJDIR)/musl-pic/src/stdio/ext2.o \
	$(OBJDIR)/musl-pic/src/stdio/fseek.o \
	$(OBJDIR)/musl-pic/src/stdio/stdin.o \
	$(OBJDIR)/musl-pic/src/stdio/fgetc.o \
	$(OBJDIR)/musl-pic/src/stdio/fread.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_read.o \
	$(OBJDIR)/musl-pic/src/stdio/__toread.o \
	$(OBJDIR)/musl-pic/src/stdio/__uflow.o \
	$(OBJDIR)/musl-pic/coreutils-shims/coreutils_shims.o \
	$(OBJDIR)/musl-pic/src/conf/fpathconf.o \
	$(OBJDIR)/musl-pic/src/conf/pathconf.o \
	$(OBJDIR)/musl-pic/src/dirent/closedir.o \
	$(OBJDIR)/musl-pic/src/dirent/dirfd.o \
	$(OBJDIR)/musl-pic/src/dirent/fdopendir.o \
	$(OBJDIR)/musl-pic/src/dirent/opendir.o \
	$(OBJDIR)/musl-pic/src/dirent/readdir.o \
	$(OBJDIR)/musl-pic/src/dirent/rewinddir.o \
	$(OBJDIR)/musl-pic/src/exit/abort_lock.o \
	$(OBJDIR)/musl-pic/src/internal/procfdname.o \
	$(OBJDIR)/musl-pic/src/process/_Fork.o \
	$(OBJDIR)/musl-pic/src/process/fork.o \
	$(OBJDIR)/musl-pic/src/stat/mkfifoat.o \
	$(OBJDIR)/musl-pic/src/stdio/fputc.o \
	$(OBJDIR)/musl-pic/src/stdio/getchar.o \
	$(OBJDIR)/musl-pic/src/stdio/puts.o \
	$(OBJDIR)/musl-pic/src/stdlib/qsort.o \
	$(OBJDIR)/musl-pic/src/stdlib/qsort_nr.o \
	$(OBJDIR)/musl-pic/src/string/mempcpy.o \
	$(OBJDIR)/musl-pic/src/string/strdup.o \
	$(OBJDIR)/musl-pic/src/string/strstr.o \
	$(OBJDIR)/musl-pic/src/unistd/chdir.o \
	$(OBJDIR)/musl-pic/src/unistd/fchdir.o \
	$(OBJDIR)/musl-pic/src/unistd/isatty.o \
	$(OBJDIR)/musl-pic/src/unistd/ftruncate.o \
	$(OBJDIR)/musl-pic/src/stdio/rename.o \
	$(OBJDIR)/musl-pic/src/signal/sigemptyset.o \
	$(OBJDIR)/musl-pic/src/ctype/isblank.o \
	$(OBJDIR)/musl-pic/src/ctype/iscntrl.o \
	$(OBJDIR)/musl-pic/src/ctype/iswalnum.o \
	$(OBJDIR)/musl-pic/src/ctype/iswalpha.o \
	$(OBJDIR)/musl-pic/src/ctype/iswblank.o \
	$(OBJDIR)/musl-pic/src/ctype/iswcntrl.o \
	$(OBJDIR)/musl-pic/src/ctype/iswctype.o \
	$(OBJDIR)/musl-pic/src/ctype/iswgraph.o \
	$(OBJDIR)/musl-pic/src/ctype/iswlower.o \
	$(OBJDIR)/musl-pic/src/ctype/iswprint.o \
	$(OBJDIR)/musl-pic/src/ctype/iswpunct.o \
	$(OBJDIR)/musl-pic/src/ctype/iswspace.o \
	$(OBJDIR)/musl-pic/src/ctype/iswupper.o \
	$(OBJDIR)/musl-pic/src/ctype/iswxdigit.o \
	$(OBJDIR)/musl-pic/src/ctype/tolower.o \
	$(OBJDIR)/musl-pic/src/ctype/toupper.o \
	$(OBJDIR)/musl-pic/src/ctype/towctrans.o \
	$(OBJDIR)/musl-pic/src/env/putenv.o \
	$(OBJDIR)/musl-pic/src/env/setenv.o \
	$(OBJDIR)/musl-pic/src/env/unsetenv.o \
	$(OBJDIR)/musl-pic/src/internal/intscan.o \
	$(OBJDIR)/musl-pic/src/internal/shgetc.o \
	$(OBJDIR)/musl-pic/src/locale/strcoll.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbsinit.o \
	$(OBJDIR)/musl-pic/src/regex/fnmatch.o \
	$(OBJDIR)/musl-pic/src/signal/sigaddset.o \
	$(OBJDIR)/musl-pic/src/signal/sigismember.o \
	$(OBJDIR)/musl-pic/src/stdio/sprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/vsprintf.o \
	$(OBJDIR)/musl-pic/src/stdlib/strtol.o \
	$(OBJDIR)/musl-pic/src/string/wcschr.o \
	$(OBJDIR)/musl-pic/src/string/wcslen.o \
	$(OBJDIR)/musl-pic/src/time/__month_to_secs.o \
	$(OBJDIR)/musl-pic/src/time/__secs_to_tm.o \
	$(OBJDIR)/musl-pic/src/time/__tm_to_secs.o \
	$(OBJDIR)/musl-pic/src/time/__tz.o \
	$(OBJDIR)/musl-pic/src/time/__year_to_secs.o \
	$(OBJDIR)/musl-pic/src/time/gmtime_r.o \
	$(OBJDIR)/musl-pic/src/time/localtime_r.o \
	$(OBJDIR)/musl-pic/src/time/mktime.o \
	$(OBJDIR)/musl-pic/src/time/strftime.o \
	$(OBJDIR)/musl-pic/src/time/timegm.o \
	$(OBJDIR)/musl-pic/src/unistd/tcgetpgrp.o \
	$(OBJDIR)/musl-pic/src/process/execve.o \
	$(OBJDIR)/musl-pic/src/unistd/pipe.o \
	$(OBJDIR)/musl-pic/src/unistd/dup.o \
	$(OBJDIR)/musl-pic/src/termios/tcgetattr.o \
	$(OBJDIR)/musl-pic/src/termios/tcsetattr.o \
	$(OBJDIR)/musl-pic/bash-shims/bash_shims.o \

# The interpreter/libc.so itself: -shared -e _dlstart is exactly
# musl's own real link line for it (see musl/Makefile's $(LDSO)
# rule) - a real ET_DYN with a real .dynsym/.dynamic/PLT/GOT, not
# something poc-os invented a shape for. build/libc.so (this exact
# name, not build/_libc.so) is also what -lc below resolves against.
# -z defs (unlike a plain "ld -shared", which tolerates an unresolved
# symbol in a shared object and only fails later, at runtime, if
# something actually calls it) makes ld fail this build step itself
# the moment MUSL_LDSO_OBJS's own object set has any real internal gap
# (confirmed necessary the hard way: an earlier musl-pic/src/stat/
# mkfifoat.o addition silently left a real, unresolved "mknodat"
# reference sitting in a built-and-installed libc.so, invisible until
# something happened to exercise that exact code path).
# Debug info is stripped for the same MAXFILE-budget reason $(BUILD)/_%
# strips it (see that rule's comment) - unstripped this is >250KB,
# stripped it's ~55KB. Installed into the image at /usr/lib/libc.so -
# see MKFS_INSTALL and $(BUILD)/fs.img below - not through UPROGS's
# usual root-placed/underscore-stripped convention.
$(BUILD)/libc.so: $(MUSL_LDSO_OBJS) | $(BUILD)
	$(LD) -m elf_x86_64 -shared -e _dlstart -z defs -o $@ $^
	$(OBJCOPY) --strip-debug $@
	$(OBJCOPY) --strip-unneeded $@

# Scrt1: musl/crt/Scrt1.c is just "#include crt1.c" (see musl/crt/
# crt1.c) - the *-fPIC compiled* variant of the same _start real musl
# uses for every dynamically-linked (non-static) executable, calling
# __libc_start_main() via the PLT like any other libc.so import
# rather than linking it in directly the way crt1.o did.

# coreutils/: real GNU coreutils 9.5 source (src/, lib/ - the latter is
# gnulib, coreutils' portability library) vendored the same way musl/
# is - unmodified upstream, except coreutils/lib/config.h and
# coreutils/lib/{error.h,stdckdint.h,configmake.h}/src/{version.h,
# version.c}, which are what a real ./configure + make run would
# normally *generate* (not source you'd hand-write) - vendored
# pre-generated instead, since this Makefile doesn't run autoconf/
# automake at all (the build machine may not even have them - see
# coreutils/poc/config.h's own comment for how these were produced:
# adapted from a native, non-cross ./configure run, not written from
# scratch). coreutils/poc/ is poc-os's own small addition on top - not
# vendored gnulib - the same idea as musl/test/ldso_stubs.c: a
# handful of functions real gnulib only ever declares inside its own
# header *replacements* (lib/wchar.in.h, lib/stdio.in.h, ...), which
# this build skips in favor of musl's real public headers directly
# (see COREUTILS_INC below), plus the handful gnulib has no
# implementation of at all for this libc (__fpending, a translating
# __fstat, a signal-free abort() - poc-os has no signal delivery of
# any kind - see coreutils/poc/coreutils_shims.c, now part of
# MUSL_LDSO_OBJS/libc.so itself rather than linked per-executable).
#
# Every coreutils/gnulib object is -fPIC (COREUTILS_PIC_CFLAGS, not a
# -fno-pie one) and linked into true/false/cat themselves as a PIE
# executable importing libc (open/printf/malloc/...) from libc.so via
# the real ELF dynamic linker - not statically linked, since that's
# what was actually asked for: real dynamic linking against /usr/lib.
#
# Mirrors MUSL_PIC_CFLAGS/INC in shape, not value - std=gnu11 (some
# coreutils source already assumes C23's nullptr/unreachable()),
# -DHAVE_CONFIG_H (every coreutils/gnulib source file starts with
# #include <config.h>), and coreutils/poc's three directories take
# priority in the -I order so its config.h/overrides are seen first.
# -include forces poc_prelude.h ahead of every source file's own first
# #include, the same role -DHAVE_CONFIG_H plays for config.h itself.
COREUTILS_PIC_CFLAGS = -std=gnu11 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -DHAVE_CONFIG_H -Os \
	-m64 -mgeneral-regs-only -fno-stack-protector -fPIC \
	-fno-omit-frame-pointer -g -Wall
COREUTILS_INC = -include coreutils/poc/poc_prelude.h -Icoreutils/poc -Icoreutils/lib -Icoreutils/src \
	-Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/coreutils-pic/%.o: coreutils/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(COREUTILS_PIC_CFLAGS) $(COREUTILS_INC) -c -o $@ $<

# The real gnulib/coreutils objects true/false need beyond libc
# itself - grown file-by-file exactly like MUSL_LIBC_OBJS/
# MUSL_LDSO_OBJS were (see their own comments): starting from "does
# coreutils/src/true.c compile" and adding whichever gnulib source
# each successive undefined reference pointed at. All of this stays
# statically linked into each executable (real gnulib isn't part of
# libc on any real system either) - only the true libc-side functions
# beyond that (COREUTILS_CAT_MUSL_OBJS's musl additions, folded into
# MUSL_LDSO_OBJS/libc.so itself instead - see that variable) come from
# the dynamic linker.
COREUTILS_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/c-ctype.o \
	$(OBJDIR)/coreutils-pic/lib/c-strcasecmp.o \
	$(OBJDIR)/coreutils-pic/lib/close-stream.o \
	$(OBJDIR)/coreutils-pic/lib/closeout.o \
	$(OBJDIR)/coreutils-pic/lib/error.o \
	$(OBJDIR)/coreutils-pic/lib/exitfail.o \
	$(OBJDIR)/coreutils-pic/lib/ialloc.o \
	$(OBJDIR)/coreutils-pic/lib/localcharset.o \
	$(OBJDIR)/coreutils-pic/lib/progname.o \
	$(OBJDIR)/coreutils-pic/lib/propername-lite.o \
	$(OBJDIR)/coreutils-pic/lib/quotearg.o \
	$(OBJDIR)/coreutils-pic/lib/version-etc.o \
	$(OBJDIR)/coreutils-pic/lib/version-etc-fsf.o \
	$(OBJDIR)/coreutils-pic/lib/xalloc-die.o \
	$(OBJDIR)/coreutils-pic/lib/xmalloc.o \
	$(OBJDIR)/coreutils-pic/src/version.o \

# true/false: PIE, -e _start via musl/crt/Scrt1.c - run as
# "runmusl true"/"runmusl false" like every other musl-crt1-style
# UPROGS entry, not directly by name.
$(BUILD)/_true: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/true.o \
		$(COREUTILS_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/true.o \
		$(COREUTILS_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/true.dis
	$(OBJCOPY) --strip-debug $@

# false.c is coreutils/src/false.c itself just "#define EXIT_STATUS
# EXIT_FAILURE" then "#include \"true.c\"" - same object set as true.
$(BUILD)/_false: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/false.o \
		$(COREUTILS_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/false.o \
		$(COREUTILS_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/false.dis
	$(OBJCOPY) --strip-debug $@

# The further gnulib objects cat.c needs beyond COREUTILS_GNULIB_OBJS's
# true/false-derived subset - grown the exact same file-by-file way,
# starting from "does coreutils/src/cat.c compile" this time. Real
# file I/O (open/read/close/fstat) is what actually distinguishes
# cat.c from true.c/false.c here, not anything cat-specific - the
# next utility with real file I/O should need few, if any, further
# gnulib additions beyond this set (any further libc-side needs go
# into MUSL_LDSO_OBJS/libc.so instead, not here).
COREUTILS_CAT_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/alignalloc.o \
	$(OBJDIR)/coreutils-pic/lib/copy-file-range.o \
	$(OBJDIR)/coreutils-pic/lib/fadvise.o \
	$(OBJDIR)/coreutils-pic/lib/full-write.o \
	$(OBJDIR)/coreutils-pic/lib/getopt.o \
	$(OBJDIR)/coreutils-pic/lib/getopt1.o \
	$(OBJDIR)/coreutils-pic/lib/safe-read.o \
	$(OBJDIR)/coreutils-pic/lib/safe-write.o \
	$(OBJDIR)/coreutils-pic/lib/xalignalloc.o \

$(BUILD)/_gcat: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cat.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cat.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/gcat.dis
	$(OBJCOPY) --strip-debug $@

# echo: like true/false/cat, every one of these utilities' main() parses
# --help/--version through gnulib's getopt_long (COREUTILS_CAT_GNULIB_OBJS'
# getopt.o/getopt1.o, despite the name - see that variable's own comment)
# even when it takes no other options, so every rule below needs it, not
# just cat's own real-file-I/O additions.
$(BUILD)/_gecho: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/echo.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/echo.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/gecho.dis
	$(OBJCOPY) --strip-debug $@

# basename/dirname: share basename-lgpl.o (the actual path-splitting
# logic); basename.c additionally needs stripslash.o (trailing-slash
# removal, which dirname doesn't do).
COREUTILS_BASENAME_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \

$(BUILD)/_basename: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/basename.o \
		$(OBJDIR)/coreutils-pic/lib/basename.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/basename.o \
		$(OBJDIR)/coreutils-pic/lib/basename.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/basename.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/_dirname: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/dirname.o \
		$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/dirname.o \
		$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/dirname.dis
	$(OBJCOPY) --strip-debug $@

# yes: long-options.o is --help/--version's shared "--help"/"--version
# alone on the command line" handling, gnulib's usual long_options()
# helper.
$(BUILD)/_yes: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/yes.o \
		$(OBJDIR)/coreutils-pic/lib/long-options.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/yes.o \
		$(OBJDIR)/coreutils-pic/lib/long-options.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/yes.dis
	$(OBJCOPY) --strip-debug $@

# head/tr/cut: all three compile and link fine (see git history if
# reviving this), but real FILE*-buffered I/O (fopen/fgetc/ungetc/
# setvbuf/...) pushes libc.so past MAXFILE. MAXFILE is no longer the
# ~70KB single-indirect ceiling it once was (see include/fs.h's own
# MAXFILE comment - kernel/fs.c's bmap()/itrunc() and mkfs/mkfs.c's
# iappend() all grew real doubly-indirect block support specifically
# because this ceiling kept blocking libc.so growth), so reviving these
# three is now plausible again - just not yet done; nobody's re-run the
# resolver against them since the filesystem was extended.

# mkdir/rmdir/rm/ln/chmod/pwd: the first coreutils additions since the
# doubly-indirect MAXFILE extension - real directory operations, which
# is also why they needed SYS_getdents/SYS_fchdir (kernel/sysfile.c) to
# exist at all (opendirat.o/fts.o's real getdents()-based directory
# traversal, not just single-file open/read/write like cat/echo).
# Object lists below came from the same resolver-driven, one-undefined-
# symbol-at-a-time process as COREUTILS_GNULIB_OBJS/
# COREUTILS_CAT_GNULIB_OBJS above.
COREUTILS_MKDIR_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/mkdir-p.o \
	$(OBJDIR)/coreutils-pic/lib/modechange.o \
	$(OBJDIR)/coreutils-pic/src/prog-fprintf.o \
	$(OBJDIR)/coreutils-pic/lib/savewd.o \
	$(OBJDIR)/coreutils-pic/lib/dirchownmod.o \
	$(OBJDIR)/coreutils-pic/lib/mkancesdirs.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_mkdir: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mkdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MKDIR_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mkdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MKDIR_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/mkdir.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_RMDIR_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/src/prog-fprintf.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \

$(BUILD)/_rmdir: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rmdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RMDIR_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rmdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RMDIR_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/rmdir.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_RM_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/src/remove.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/write-any-file.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/file-type.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/fts.o \
	$(OBJDIR)/coreutils-pic/lib/xfts.o \
	$(OBJDIR)/coreutils-pic/lib/c-file-type.o \
	$(OBJDIR)/coreutils-pic/lib/cycle-check.o \
	$(OBJDIR)/coreutils-pic/lib/i-ring.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_rm: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rm.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RM_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rm.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RM_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/rm.dis
	$(OBJCOPY) --strip-debug $@

# scratch_buffer_grow.o/scratch_buffer_grow_preserve.o genuinely live at
# coreutils/lib/malloc/ (not coreutils/lib/ directly, unlike everything
# else in this list) - $(OBJDIR)/coreutils-pic/%.o: coreutils/%.c's
# pattern rule mirrors that nesting, so these two are the only entries
# below with a lib/malloc/ path component rather than a flat lib/ one.
COREUTILS_LN_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/unlinkdir.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/lib/dirname.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/backup-find.o \
	$(OBJDIR)/coreutils-pic/src/force-link.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/src/relpath.o \
	$(OBJDIR)/coreutils-pic/lib/same.o \
	$(OBJDIR)/coreutils-pic/lib/backupfile.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/renameatu.o \
	$(OBJDIR)/coreutils-pic/lib/tempname.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_ln: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ln.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LN_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ln.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LN_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/ln.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_CHMOD_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/xfts.o \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/lib/modechange.o \
	$(OBJDIR)/coreutils-pic/lib/fts.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/cycle-check.o \
	$(OBJDIR)/coreutils-pic/lib/i-ring.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_chmod: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/chmod.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CHMOD_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/chmod.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CHMOD_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/chmod.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_PWD_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/lib/xgetcwd.o \

$(BUILD)/_pwd: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/pwd.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_PWD_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/pwd.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_PWD_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/pwd.dis
	$(OBJCOPY) --strip-debug $@

# mv/cp: needed a real SYS_ftruncate (include/syscall.h, kernel/fs.c's
# itruncto(), kernel/sysfile.c's sys_ftruncate()) - coreutils/src/
# copy.c calls a real ftruncate() to record a sparse copy's final
# length - plus qcopy_acl()/qset_acl() actually linking, which turned
# out to be a config.h problem, not a missing-implementation one:
# coreutils/poc/config.h's USE_ACL/HAVE_SYS_ACL_H were both left at the
# values a native macOS ./configure run detected (macOS has its own
# <sys/acl.h>), pulling in gnulib's real ACL codepath and its acl_t
# type poc-os has no definition for. Flipped both off (see that file's
# own history) so gnulib's own no-ACL fallback path (plain chmod, via
# coreutils/lib/{qcopy,qset}-acl.c's USE_ACL==0 branch) compiles
# instead - the same "config.h assumed the wrong host" class of fix as
# HAVE_FCLONEFILEAT above, not a new shim.
COREUTILS_MV_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/src/copy.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/src/cp-hash.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/renameatu.o \
	$(OBJDIR)/coreutils-pic/src/remove.o \
	$(OBJDIR)/coreutils-pic/lib/backupfile.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/targetdir.o \
	$(OBJDIR)/coreutils-pic/lib/backup-find.o \
	$(OBJDIR)/coreutils-pic/lib/areadlink-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/areadlinkat-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/backup-rename.o \
	$(OBJDIR)/coreutils-pic/lib/buffer-lcm.o \
	$(OBJDIR)/coreutils-pic/lib/write-any-file.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/copy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/dirname.o \
	$(OBJDIR)/coreutils-pic/lib/fdutimensat.o \
	$(OBJDIR)/coreutils-pic/lib/file-type.o \
	$(OBJDIR)/coreutils-pic/src/force-link.o \
	$(OBJDIR)/coreutils-pic/lib/chmodat.o \
	$(OBJDIR)/coreutils-pic/lib/chownat.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/same-inode.o \
	$(OBJDIR)/coreutils-pic/lib/qset-acl.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/lib/fts.o \
	$(OBJDIR)/coreutils-pic/lib/same.o \
	$(OBJDIR)/coreutils-pic/lib/savedir.o \
	$(OBJDIR)/coreutils-pic/lib/set-acl.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/utimecmp.o \
	$(OBJDIR)/coreutils-pic/lib/xfts.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/c-file-type.o \
	$(OBJDIR)/coreutils-pic/lib/cycle-check.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/acl-internal.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/i-ring.o \
	$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/opendir-safer.o \
	$(OBJDIR)/coreutils-pic/lib/qcopy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/set-permissions.o \
	$(OBJDIR)/coreutils-pic/lib/tempname.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \
	$(OBJDIR)/coreutils-pic/lib/get-permissions.o \

$(BUILD)/_mv: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mv.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MV_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mv.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MV_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/mv.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_CP_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/src/copy.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/lib/copy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/backup-find.o \
	$(OBJDIR)/coreutils-pic/src/cp-hash.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/chmodat.o \
	$(OBJDIR)/coreutils-pic/lib/chownat.o \
	$(OBJDIR)/coreutils-pic/lib/backupfile.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/targetdir.o \
	$(OBJDIR)/coreutils-pic/lib/areadlink-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/areadlinkat-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/backup-rename.o \
	$(OBJDIR)/coreutils-pic/lib/buffer-lcm.o \
	$(OBJDIR)/coreutils-pic/lib/write-any-file.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/dirname.o \
	$(OBJDIR)/coreutils-pic/lib/fdutimensat.o \
	$(OBJDIR)/coreutils-pic/src/force-link.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/same-inode.o \
	$(OBJDIR)/coreutils-pic/lib/qcopy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/qset-acl.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/lib/renameatu.o \
	$(OBJDIR)/coreutils-pic/lib/same.o \
	$(OBJDIR)/coreutils-pic/lib/savedir.o \
	$(OBJDIR)/coreutils-pic/lib/set-acl.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/utimecmp.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/acl-internal.o \
	$(OBJDIR)/coreutils-pic/lib/get-permissions.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/opendir-safer.o \
	$(OBJDIR)/coreutils-pic/lib/set-permissions.o \
	$(OBJDIR)/coreutils-pic/lib/tempname.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_cp: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cp.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CP_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cp.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CP_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/cp.dis
	$(OBJCOPY) --strip-debug $@

# ls: needed real getdents (SYS_getdents/opendir - already added for
# mkdir/rm/etc's fts.c above) plus three gnulib functions this port has
# no working real implementation of at all - not shimmed as no-ops,
# genuinely reimplemented:
#   - human_options()/human_readable() (coreutils/poc/human_shim.c):
#     real gnulib (lib/human.c) formats sizes with `long double`
#     arithmetic even in the plain, non "-h" case - needs the x87 FPU,
#     unavailable under -mgeneral-regs-only (see this file's own
#     COREUTILS_PIC_CFLAGS comment) for the same reason gnulib's
#     hash.c did (coreutils_shims.c's own hash_initialize comment).
#     Plain 64-bit integer arithmetic throughout instead - exact, not
#     an approximation, for any size poc-os's own MAXFILE could ever
#     actually produce.
#   - timespec_cmp() (coreutils/poc/timespec_shim.c): timespec.h
#     declares this as a plain C99 "inline" (needs one real external
#     instantiation to exist - same rule chmodat()/chownat()/
#     psame_inode() already needed one for), but the file gnulib
#     provides for that (lib/timespec.c) forces every OTHER inline in
#     the same header to instantiate too, including timespectod() -
#     which needs the FPU for the exact same reason human_readable()
#     does. A tiny replacement providing just the one function ls -l
#     actually calls sidesteps the FPU entirely.
# Also needed config.h's USE_ACL/HAVE_SYS_ACL_H fix (mv/cp's own
# comment above), SETLOCALE_NULL_MAX/setlocale_null_r and c32iscntrl/
# c32width declarations (poc_prelude.h - gnulib's own header
# replacements are the only place that normally declare these), and
# sigaction()/sigprocmask()/signal()/getpwnam()/getpwuid()/getgrnam()/
# getgrgid()/gethostname() (coreutils_shims.c - poc-os has no signal
# delivery or user/group database of any kind, so "always succeeded,
# nothing to report" is the accurate answer, not a fake success).
COREUTILS_LS_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/obstack.o \
	$(OBJDIR)/coreutils-pic/lib/areadlink-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/c-strncasecmp.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/file-has-acl.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/filevercmp.o \
	$(OBJDIR)/coreutils-pic/lib/idcache.o \
	$(OBJDIR)/coreutils-pic/lib/gettime.o \
	$(OBJDIR)/coreutils-pic/lib/hard-locale.o \
	$(OBJDIR)/coreutils-pic/poc/human_shim.o \
	$(OBJDIR)/coreutils-pic/lib/imaxtostr.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/time_rz.o \
	$(OBJDIR)/coreutils-pic/src/ls-dir.o \
	$(OBJDIR)/coreutils-pic/lib/mbswidth.o \
	$(OBJDIR)/coreutils-pic/lib/mpsort.o \
	$(OBJDIR)/coreutils-pic/lib/nstrftime.o \
	$(OBJDIR)/coreutils-pic/poc/timespec_shim.o \
	$(OBJDIR)/coreutils-pic/lib/umaxtostr.o \
	$(OBJDIR)/coreutils-pic/lib/xgethostname.o \
	$(OBJDIR)/coreutils-pic/lib/xdectoumax.o \
	$(OBJDIR)/coreutils-pic/lib/xstrtol-error.o \
	$(OBJDIR)/coreutils-pic/lib/xstrtoumax.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/lib/setlocale_null.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/setlocale_null-unlocked.o \

$(BUILD)/_ls: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ls.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LS_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ls.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LS_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/ls.dis
	$(OBJCOPY) --strip-debug $@

# bash/: real GNU bash 5.2 source, vendored the same way musl/ and
# coreutils/ are (unmodified upstream) - bash/poc/ is poc-os's own
# addition on top, same idea as coreutils/poc/: config.h (normally
# produced by bash's own ./configure, here adapted from a real native,
# non-cross ./configure + make run - see bash/poc/config.h's presence
# for how HAVE_DLOPEN/HAVE_ICONV/HAVE_SYSLOG/HAVE_LIBDL/
# HAVE_LOCALE_CHARSET got turned back off for poc-os, and JOB_CONTROL/
# HISTORY/readline are off because --disable-job-control/--disable-
# history/--disable-readline were passed to that native configure -
# job control needs process groups/sessions/tcsetpgrp poc-os's kernel
# doesn't have, and readline needs a termcap/terminfo library this
# build has none of), pathnames.h/version.h/pipesize.h (pipesize.h
# hand-corrected to poc-os's own real kernel/pipe.c PIPESIZE, 512 -
# the native run's own probe measured *macOS's* pipe buffer, 64KB,
# which would just be a wrong answer for `ulimit -p` here), syntax.c
# and builtins.c/builtext.h (host-independent generated code, reused
# as-is), bash/poc/builtins/*.c (also generated - by builtins/
# mkbuiltins from builtins/*.def, which bash's own build deletes right
# after compiling in the ordinary case; regenerated here via a direct
# ./mkbuiltins invocation instead so they persist), and signames.h/
# lsignames.h (NOT reused from the native run - mksignames normally
# enumerates the *host's* <signal.h>, so a native run bakes in macOS's
# BSD-flavored signal set (SIGEMT, SIGINFO, ...); hand-derived instead
# from musl/arch/x86_64/bits/signal.h's real SIGHUP..SIGSYS numbering
# plus the standard glibc/musl SIGRTMIN=34/SIGRTMAX=64 real-time-signal
# convention - see bash/poc/signames.h's own comment). bash/poc/
# bash_prelude.h/bash_shims.c fill the remaining real libc-shaped gaps
# (dup2(), uname(), getrlimit()/setrlimit(), times() - poc-os has no
# syscall for any of these), the same role coreutils/poc/
# coreutils_shims.c plays for coreutils - part of libc.so itself (see
# MUSL_LDSO_OBJS above), not linked per-executable.
#
# job control disabled (see config.h above) means bash/nojobs.c (the
# stub job-control engine), not bash/jobs.c, is what actually gets
# built into BASH_OBJS below - matching bash's own JOBS_O=nojobs.o
# selection for this exact configuration.
#
# Every bash/gnulib-style object here is -fPIC (BASH_PIC_CFLAGS) and
# linked into build/_bash as a real PIE executable importing libc from
# libc.so via the ELF dynamic linker, exactly like coreutils'
# COREUTILS_PIC_CFLAGS/true/false/cat/etc above - not statically
# linked. This -fPIC+libc.so recipe (Scrt1.o + object set + -pie
# --dynamic-linker /usr/lib/libc.so -lc) is now poc-os's *default*
# convention for any future userland software, not just bash/
# coreutils - the static ULIB/xv6-native path (user/*.c, include/
# user.h) is legacy, kept only until user/sh.c itself is retired.
#
# -DCONF_HOSTTYPE/-DCONF_OSTYPE/-DCONF_MACHTYPE: bash/conftypes.h
# expects these predefined (normally by configure, from config.h's own
# uname-derived guess) to build $HOSTTYPE/$OSTYPE/$MACHTYPE - poc-os's
# own identity, not a borrowed host one.
# -Wno-error=implicit-function-declaration: bash/parse.y's own 'j' (job
# count) prompt-expansion case calls count_all_jobs() (bash/nojobs.c -
# JOB_CONTROL is off, see config.h - really does define it) without
# #include "jobs.h" in scope - a real, harmless upstream gap (GCC 16
# defaults this to a hard error; older GCC/the native macOS build above
# only ever warned, which is why nobody upstream noticed).
BASH_PIC_CFLAGS = -std=gnu11 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -DHAVE_CONFIG_H -DSHELL -Os \
	-DCONF_HOSTTYPE='"x86_64"' -DCONF_OSTYPE='"poc-os"' -DCONF_MACHTYPE='"x86_64-poc-os"' \
	-m64 -mgeneral-regs-only -fno-stack-protector -fPIC \
	-fno-omit-frame-pointer -g -Wall -Wno-parentheses -Wno-format-security \
	-Wno-error=implicit-function-declaration -Wno-error=implicit-int
BASH_INC = -include bash/poc/bash_prelude.h \
	-Ibash/poc -Ibash/poc/builtins -Ibash -Ibash/include -Ibash/builtins -Ibash/lib -Ibash/lib/sh -Ibash/lib/glob -Ibash/lib/tilde \
	-Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/bash-pic/%.o: bash/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

$(OBJDIR)/bash-pic/poc/%.o: bash/poc/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

$(OBJDIR)/bash-pic/poc/builtins/%.o: bash/poc/builtins/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

# Core shell engine (the top-level bash/*.c files the native build's
# own final link line pulled in for this exact config - see this
# section's own comment).
BASH_CORE_OBJS = \
	$(OBJDIR)/bash-pic/shell.o \
	$(OBJDIR)/bash-pic/eval.o \
	$(OBJDIR)/bash-pic/y.tab.o \
	$(OBJDIR)/bash-pic/general.o \
	$(OBJDIR)/bash-pic/make_cmd.o \
	$(OBJDIR)/bash-pic/print_cmd.o \
	$(OBJDIR)/bash-pic/dispose_cmd.o \
	$(OBJDIR)/bash-pic/execute_cmd.o \
	$(OBJDIR)/bash-pic/variables.o \
	$(OBJDIR)/bash-pic/copy_cmd.o \
	$(OBJDIR)/bash-pic/error.o \
	$(OBJDIR)/bash-pic/expr.o \
	$(OBJDIR)/bash-pic/flags.o \
	$(OBJDIR)/bash-pic/nojobs.o \
	$(OBJDIR)/bash-pic/subst.o \
	$(OBJDIR)/bash-pic/hashcmd.o \
	$(OBJDIR)/bash-pic/hashlib.o \
	$(OBJDIR)/bash-pic/mailcheck.o \
	$(OBJDIR)/bash-pic/trap.o \
	$(OBJDIR)/bash-pic/input.o \
	$(OBJDIR)/bash-pic/unwind_prot.o \
	$(OBJDIR)/bash-pic/pathexp.o \
	$(OBJDIR)/bash-pic/sig.o \
	$(OBJDIR)/bash-pic/test.o \
	$(OBJDIR)/bash-pic/version.o \
	$(OBJDIR)/bash-pic/alias.o \
	$(OBJDIR)/bash-pic/array.o \
	$(OBJDIR)/bash-pic/arrayfunc.o \
	$(OBJDIR)/bash-pic/assoc.o \
	$(OBJDIR)/bash-pic/braces.o \
	$(OBJDIR)/bash-pic/bracecomp.o \
	$(OBJDIR)/bash-pic/bashhist.o \
	$(OBJDIR)/bash-pic/bashline.o \
	$(OBJDIR)/bash-pic/list.o \
	$(OBJDIR)/bash-pic/stringlib.o \
	$(OBJDIR)/bash-pic/locale.o \
	$(OBJDIR)/bash-pic/findcmd.o \
	$(OBJDIR)/bash-pic/redir.o \
	$(OBJDIR)/bash-pic/pcomplete.o \
	$(OBJDIR)/bash-pic/pcomplib.o \
	$(OBJDIR)/bash-pic/xmalloc.o \
	$(OBJDIR)/bash-pic/poc/syntax.o \
	$(OBJDIR)/bash-pic/poc/builtins.o \

# builtins/: the four hand-written (not .def-generated) support files
# bash's own tarball ships pristine, plus every real builtin command
# (bash/poc/builtins/*.c - .def-generated, see this section's own
# comment for why they're regenerated into bash/poc/ instead of
# bash/builtins/ directly).
BASH_BUILTINS_OBJS = \
	$(OBJDIR)/bash-pic/builtins/common.o \
	$(OBJDIR)/bash-pic/builtins/evalfile.o \
	$(OBJDIR)/bash-pic/builtins/evalstring.o \
	$(OBJDIR)/bash-pic/builtins/bashgetopt.o \
	$(OBJDIR)/bash-pic/builtins/getopt.o \
	$(OBJDIR)/bash-pic/poc/builtins/alias.o \
	$(OBJDIR)/bash-pic/poc/builtins/bind.o \
	$(OBJDIR)/bash-pic/poc/builtins/break.o \
	$(OBJDIR)/bash-pic/poc/builtins/builtin.o \
	$(OBJDIR)/bash-pic/poc/builtins/caller.o \
	$(OBJDIR)/bash-pic/poc/builtins/cd.o \
	$(OBJDIR)/bash-pic/poc/builtins/colon.o \
	$(OBJDIR)/bash-pic/poc/builtins/command.o \
	$(OBJDIR)/bash-pic/poc/builtins/declare.o \
	$(OBJDIR)/bash-pic/poc/builtins/echo.o \
	$(OBJDIR)/bash-pic/poc/builtins/enable.o \
	$(OBJDIR)/bash-pic/poc/builtins/eval.o \
	$(OBJDIR)/bash-pic/poc/builtins/exec.o \
	$(OBJDIR)/bash-pic/poc/builtins/exit.o \
	$(OBJDIR)/bash-pic/poc/builtins/fc.o \
	$(OBJDIR)/bash-pic/poc/builtins/fg_bg.o \
	$(OBJDIR)/bash-pic/poc/builtins/getopts.o \
	$(OBJDIR)/bash-pic/poc/builtins/hash.o \
	$(OBJDIR)/bash-pic/poc/builtins/help.o \
	$(OBJDIR)/bash-pic/poc/builtins/kill.o \
	$(OBJDIR)/bash-pic/poc/builtins/let.o \
	$(OBJDIR)/bash-pic/poc/builtins/mapfile.o \
	$(OBJDIR)/bash-pic/poc/builtins/printf.o \
	$(OBJDIR)/bash-pic/poc/builtins/pushd.o \
	$(OBJDIR)/bash-pic/poc/builtins/read.o \
	$(OBJDIR)/bash-pic/poc/builtins/return.o \
	$(OBJDIR)/bash-pic/poc/builtins/set.o \
	$(OBJDIR)/bash-pic/poc/builtins/setattr.o \
	$(OBJDIR)/bash-pic/poc/builtins/shift.o \
	$(OBJDIR)/bash-pic/poc/builtins/shopt.o \
	$(OBJDIR)/bash-pic/poc/builtins/source.o \
	$(OBJDIR)/bash-pic/poc/builtins/suspend.o \
	$(OBJDIR)/bash-pic/poc/builtins/test.o \
	$(OBJDIR)/bash-pic/poc/builtins/times.o \
	$(OBJDIR)/bash-pic/poc/builtins/trap.o \
	$(OBJDIR)/bash-pic/poc/builtins/type.o \
	$(OBJDIR)/bash-pic/poc/builtins/ulimit.o \
	$(OBJDIR)/bash-pic/poc/builtins/umask.o \
	$(OBJDIR)/bash-pic/poc/builtins/wait.o \
	$(OBJDIR)/bash-pic/poc/builtins/complete.o \

# lib/sh, lib/glob, lib/tilde: bash's own portability/pattern-matching/
# ~-expansion support libraries (the real gnulib-equivalent bash ships
# itself) - member lists taken directly from the native run's own
# libsh.a/libglob.a/libtilde.a (no lib/readline/libhistory.a: HISTORY
# is off - see config.h - so bash's own code never calls into it).
BASH_LIB_OBJS = \
	$(OBJDIR)/bash-pic/lib/sh/clktck.o \
	$(OBJDIR)/bash-pic/lib/sh/clock.o \
	$(OBJDIR)/bash-pic/lib/sh/getenv.o \
	$(OBJDIR)/bash-pic/lib/sh/oslib.o \
	$(OBJDIR)/bash-pic/lib/sh/setlinebuf.o \
	$(OBJDIR)/bash-pic/lib/sh/strnlen.o \
	$(OBJDIR)/bash-pic/lib/sh/itos.o \
	$(OBJDIR)/bash-pic/lib/sh/zread.o \
	$(OBJDIR)/bash-pic/lib/sh/zwrite.o \
	$(OBJDIR)/bash-pic/lib/sh/shtty.o \
	$(OBJDIR)/bash-pic/lib/sh/shmatch.o \
	$(OBJDIR)/bash-pic/lib/sh/eaccess.o \
	$(OBJDIR)/bash-pic/lib/sh/netconn.o \
	$(OBJDIR)/bash-pic/lib/sh/netopen.o \
	$(OBJDIR)/bash-pic/lib/sh/timeval.o \
	$(OBJDIR)/bash-pic/lib/sh/makepath.o \
	$(OBJDIR)/bash-pic/lib/sh/pathcanon.o \
	$(OBJDIR)/bash-pic/lib/sh/pathphys.o \
	$(OBJDIR)/bash-pic/lib/sh/tmpfile.o \
	$(OBJDIR)/bash-pic/lib/sh/stringlist.o \
	$(OBJDIR)/bash-pic/lib/sh/stringvec.o \
	$(OBJDIR)/bash-pic/lib/sh/spell.o \
	$(OBJDIR)/bash-pic/lib/sh/shquote.o \
	$(OBJDIR)/bash-pic/lib/sh/strtrans.o \
	$(OBJDIR)/bash-pic/lib/sh/snprintf.o \
	$(OBJDIR)/bash-pic/lib/sh/mailstat.o \
	$(OBJDIR)/bash-pic/lib/sh/fmtulong.o \
	$(OBJDIR)/bash-pic/lib/sh/fmtullong.o \
	$(OBJDIR)/bash-pic/lib/sh/fmtumax.o \
	$(OBJDIR)/bash-pic/lib/sh/zcatfd.o \
	$(OBJDIR)/bash-pic/lib/sh/zmapfd.o \
	$(OBJDIR)/bash-pic/lib/sh/winsize.o \
	$(OBJDIR)/bash-pic/lib/sh/wcsdup.o \
	$(OBJDIR)/bash-pic/lib/sh/fpurge.o \
	$(OBJDIR)/bash-pic/lib/sh/zgetline.o \
	$(OBJDIR)/bash-pic/lib/sh/mbscmp.o \
	$(OBJDIR)/bash-pic/lib/sh/uconvert.o \
	$(OBJDIR)/bash-pic/lib/sh/ufuncs.o \
	$(OBJDIR)/bash-pic/lib/sh/casemod.o \
	$(OBJDIR)/bash-pic/lib/sh/input_avail.o \
	$(OBJDIR)/bash-pic/lib/sh/mbscasecmp.o \
	$(OBJDIR)/bash-pic/lib/sh/fnxform.o \
	$(OBJDIR)/bash-pic/lib/sh/unicode.o \
	$(OBJDIR)/bash-pic/lib/sh/shmbchar.o \
	$(OBJDIR)/bash-pic/lib/sh/strvis.o \
	$(OBJDIR)/bash-pic/lib/sh/utf8.o \
	$(OBJDIR)/bash-pic/lib/sh/random.o \
	$(OBJDIR)/bash-pic/lib/sh/gettimeofday.o \
	$(OBJDIR)/bash-pic/lib/sh/timers.o \
	$(OBJDIR)/bash-pic/lib/sh/wcsnwidth.o \
	$(OBJDIR)/bash-pic/lib/sh/mktime.o \
	$(OBJDIR)/bash-pic/lib/sh/mbschr.o \
	$(OBJDIR)/bash-pic/lib/sh/strtoimax.o \
	$(OBJDIR)/bash-pic/lib/glob/glob.o \
	$(OBJDIR)/bash-pic/lib/glob/strmatch.o \
	$(OBJDIR)/bash-pic/lib/glob/smatch.o \
	$(OBJDIR)/bash-pic/lib/glob/xmbsrtowcs.o \
	$(OBJDIR)/bash-pic/lib/glob/gmisc.o \
	$(OBJDIR)/bash-pic/lib/tilde/tilde.o \

BASH_OBJS = $(BASH_CORE_OBJS) $(BASH_BUILTINS_OBJS) $(BASH_LIB_OBJS)

$(BUILD)/_bash: $(OBJDIR)/musl-pic/crt/Scrt1.o $(BASH_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(BASH_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/bash.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/mkfs: mkfs/mkfs.c include/fs.h | $(BUILD)
	# -iquote (not -I) so quoted poc headers resolve to include/ while
	# <fcntl.h> etc still resolve to the host's system headers.
	gcc -Werror -Wall -iquote include -o $(BUILD)/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: $(OBJDIR)/user/%.o $(OBJDIR)/kernel/%.o $(OBJDIR)/boot/%.o $(OBJDIR)/musl/%.o $(OBJDIR)/musl-test/%.o $(OBJDIR)/musl-pic/%.o $(OBJDIR)/coreutils-pic/%.o $(OBJDIR)/bash-pic/%.o

# UPROGS is mkfs/mkfs.c's usual root-placed/underscore-stripped
# convention (a bare host path, e.g. build/_foo -> installed as /foo -
# see mkfs.c's own comment on argv). Empty today: every current binary,
# including init/sh, is instead installed explicitly under /usr/bin via
# MKFS_INSTALL's "imgpath:hostpath" form below, so there's exactly one
# place binaries live rather than some at / and some at /usr/bin. Kept
# (rather than deleted) as the mechanism a future root-placed program
# would still use.
UPROGS=\

# init/sh: not "commands" in the usual sense - init is PID 1 (the
# kernel loads initcode.asm/initcode64.asm, which SYS_execs this exact
# path - see those files' own "init:" string) and sh is what init execs
# in turn (see user/init.c) - but installed under /usr/bin like
# everything else rather than carved out as a root-level exception.
# Unlike the coreutils additions below, these apply to every ARCH: the
# 32-bit build still needs an init/sh, even with no musl/coreutils in
# it at all.
MKFS_INSTALL = usr/bin/init:$(BUILD)/_init usr/bin/sh:$(BUILD)/_sh
MKFS_INSTALL_DEPS = $(BUILD)/_init $(BUILD)/_sh

# GNU coreutils ports (true/false/cat/echo/basename/dirname/yes,
# runmusl - a manual musl-crt1-style launcher, see musl-test/%.o's own
# comment): x86_64-only, like the rest of the musl port, so conditional
# on ARCH rather than always built. libc.so has to live at exactly
# /usr/lib/libc.so - that path is baked into every one of these
# binaries' own PT_INTERP segment (--dynamic-linker /usr/lib/libc.so
# above) as the dynamic linker to load. Kept deliberately small - see
# $(BUILD)/_ghead's own comment above for why head/tr/cut aren't here
# too (MAXFILE).
ifeq ($(ARCH),64)
MKFS_INSTALL += usr/lib/libc.so:$(BUILD)/libc.so usr/bin/true:$(BUILD)/_true \
	usr/bin/false:$(BUILD)/_false usr/bin/cat:$(BUILD)/_gcat \
	usr/bin/echo:$(BUILD)/_gecho usr/bin/basename:$(BUILD)/_basename \
	usr/bin/dirname:$(BUILD)/_dirname usr/bin/yes:$(BUILD)/_yes \
	usr/bin/runmusl:$(BUILD)/_runmusl
MKFS_INSTALL_DEPS += $(BUILD)/libc.so $(BUILD)/_true $(BUILD)/_false \
	$(BUILD)/_gcat $(BUILD)/_gecho $(BUILD)/_basename $(BUILD)/_dirname \
	$(BUILD)/_yes $(BUILD)/_runmusl

# mkdir/rmdir/rm/ln/chmod/pwd: the first additions since include/fs.h's
# doubly-indirect MAXFILE extension - see that comment and each
# COREUTILS_*_GNULIB_OBJS variable's own comment above.
MKFS_INSTALL += usr/bin/mkdir:$(BUILD)/_mkdir usr/bin/rmdir:$(BUILD)/_rmdir \
	usr/bin/rm:$(BUILD)/_rm usr/bin/ln:$(BUILD)/_ln \
	usr/bin/chmod:$(BUILD)/_chmod usr/bin/pwd:$(BUILD)/_pwd
MKFS_INSTALL_DEPS += $(BUILD)/_mkdir $(BUILD)/_rmdir $(BUILD)/_rm \
	$(BUILD)/_ln $(BUILD)/_chmod $(BUILD)/_pwd

# mv/cp: needed real ftruncate() (SYS_ftruncate, kernel/fs.c's
# itruncto()) plus config.h's USE_ACL/HAVE_SYS_ACL_H fixes - see
# COREUTILS_MV_GNULIB_OBJS's own comment above.
MKFS_INSTALL += usr/bin/mv:$(BUILD)/_mv usr/bin/cp:$(BUILD)/_cp
MKFS_INSTALL_DEPS += $(BUILD)/_mv $(BUILD)/_cp

# ls: real directory listing - see COREUTILS_LS_GNULIB_OBJS's own
# comment above for what this needed beyond the mkdir/rm/etc batch's
# getdents/opendir infrastructure.
MKFS_INSTALL += usr/bin/ls:$(BUILD)/_ls
MKFS_INSTALL_DEPS += $(BUILD)/_ls

# bash: dynamically linked (Scrt1.o + libc.so, PIE) the same way as
# every coreutils entry above - see $(BUILD)/_bash's own comment for
# what this needed beyond the musl/coreutils infrastructure already
# built out.
MKFS_INSTALL += usr/bin/bash:$(BUILD)/_bash
MKFS_INSTALL_DEPS += $(BUILD)/_bash

endif

$(BUILD)/fs.img: $(BUILD)/mkfs $(UPROGS) $(MKFS_INSTALL_DEPS)
	./$(BUILD)/mkfs $(BUILD)/fs.img $(UPROGS) $(MKFS_INSTALL)

-include $(OBJDIR)/boot/*.d $(OBJDIR)/kernel/*.d $(OBJDIR)/user/*.d

all: $(BUILD)/poc.img $(BUILD)/fs.img

run: all
	$(QEMU) $(QEMUOPTS) </dev/null >/dev/null 2>&1 &

clean:
	rm -rf $(BUILD)
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg .gdbinit

# make a printout
FILES = $(shell grep -v '^\#' tools/runoff.list)
PRINT = tools/runoff.list tools/runoff.spec README docs/toc.hdr docs/toc.ftr $(FILES)

poc.pdf: $(PRINT)
	./tools/runoff
	ls -l poc.pdf

print: poc.pdf

# run in emulators

bochs : $(BUILD)/fs.img $(BUILD)/poc.img
	if [ ! -e .bochsrc ]; then ln -s dot-bochsrc .bochsrc; fi
	bochs -q

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 2
endif
QEMUOPTS = -drive file=$(BUILD)/fs.img,index=1,media=disk,format=raw -drive file=$(BUILD)/poc.img,index=0,media=disk,format=raw -smp $(CPUS) -m 512 $(QEMUEXTRA)

# `run` launches QEMU detached from this shell's stdio (</dev/null so
# it can't be suspended by SIGTTIN when backgrounded, output silenced)
# so it opens its own GUI window and the terminal is free again
# immediately, instead of blocking until QEMU exits the way `qemu`
# below does. Serial console and monitor fall back to virtual-console
# tabs inside that window (Ctrl-Alt-2/3).
qemu: $(BUILD)/fs.img $(BUILD)/poc.img
	$(QEMU) -serial mon:stdio $(QEMUOPTS)

qemu-memfs: $(BUILD)/pocmemfs.img
	$(QEMU) -drive file=$(BUILD)/pocmemfs.img,index=0,media=disk,format=raw -smp $(CPUS) -m 256

qemu-nox: $(BUILD)/fs.img $(BUILD)/poc.img
	$(QEMU) -nographic $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl
	sed "s/localhost:1234/localhost:$(GDBPORT)/" < $^ > $@

qemu-gdb: $(BUILD)/fs.img $(BUILD)/poc.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -serial mon:stdio $(QEMUOPTS) -S $(QEMUGDB)

qemu-nox-gdb: $(BUILD)/fs.img $(BUILD)/poc.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -nographic $(QEMUOPTS) -S $(QEMUGDB)

# CUT HERE
# prepare dist for students
# after running make dist, probably want to
# rename it to rev0 or rev1 or so on and then
# check in that version.

EXTRA=\
	mkfs/mkfs.c user/ulib.c include/user.h\
	user/printf.c user/umalloc.c\
	README dot-bochsrc tools/*.pl docs/toc.* tools/runoff tools/runoff1 tools/runoff.list\
	.gdbinit.tmpl tools/gdbutil\

dist:
	rm -rf dist
	mkdir dist
	for i in $(FILES); \
	do \
		grep -v PAGEBREAK $$i >dist/$$i; \
	done
	sed '/CUT HERE/,$$d' Makefile >dist/Makefile
	echo >dist/tools/runoff.spec
	cp $(EXTRA) dist

dist-test:
	rm -rf dist
	make dist
	rm -rf dist-test
	mkdir dist-test
	cp dist/* dist-test
	cd dist-test; $(MAKE) print
	cd dist-test; $(MAKE) bochs || true
	cd dist-test; $(MAKE) qemu

# update this rule (change rev#) when it is time to
# make a new revision.
tar:
	rm -rf /tmp/poc
	mkdir -p /tmp/poc
	cp dist/* dist/.gdbinit.tmpl /tmp/poc
	(cd /tmp; tar cf - poc) | gzip >poc-rev10.tar.gz  # the next one will be 10 (9/17)

.PHONY: all run clean dist-test dist tags print
