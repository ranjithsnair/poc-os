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

$(BUILD)/poc.img: $(BUILD)/bootblock $(BUILD)/kernel | $(BUILD)
	dd if=/dev/zero of=$(BUILD)/poc.img count=10000
	dd if=$(BUILD)/bootblock of=$(BUILD)/poc.img conv=notrunc
	dd if=$(BUILD)/kernel of=$(BUILD)/poc.img seek=1 conv=notrunc

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

$(BUILD)/_forktest: $(OBJDIR)/user/forktest.o $(ULIB) | $(BUILD)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $(BUILD)/_forktest $(OBJDIR)/user/forktest.o $(OBJDIR)/user/ulib.o $(USYSOBJ) $(X86ASMOBJ)
	$(OBJDUMP) -S $(BUILD)/_forktest > $(BUILD)/forktest.dis
	$(OBJCOPY) --strip-debug $(BUILD)/_forktest

# musl-test/: a standalone smoke test for the musl port (see musl/README
# and musl/test/hello.c) - it calls the vendored, poc-os-forked
# arch/x86_64/syscall_arch.h directly, with none of the rest of musl
# involved yet, so unlike ordinary user/ programs it needs neither ULIB
# nor an -e main convention shared with xv6's own ulib.c (it doesn't
# link ulib.c at all). -Iinclude is here too (not just
# -Imusl/arch/x86_64) since some of these tests reference poc-os kernel
# constants directly (e.g. ARCH_SET_FS from include/syscall.h) rather
# than only musl's own headers.
$(OBJDIR)/musl-test/%.o: musl/test/%.c | $(OBJDIR)/musl-test
	$(CC) $(CFLAGS) -Imusl/arch/x86_64 -Iinclude -c -o $@ $<

$(BUILD)/_muslhello: $(OBJDIR)/musl-test/hello.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/muslhello.dis
	$(OBJCOPY) --strip-debug $@

# execve_launch/execve_verify: the two-binary SYS_execve smoke test
# (see musl/test/execve_launch.c). execve_launch is entered the normal
# poc-os way (-e main); execve_verify is entered the Linux/musl way
# (-e _start_asm, argc/argv/envp/auxv decoded from %rsp, not %rdi/%rsi)
# to check what kernel/exec.c's execve() actually put on its stack.
$(BUILD)/_execve_launch: $(OBJDIR)/musl-test/execve_launch.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/execve_launch.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/_execve_verify: $(OBJDIR)/musl-test/execve_verify.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e _start_asm -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/execve_verify.dis
	$(OBJCOPY) --strip-debug $@

# tls: SYS_arch_prctl(ARCH_SET_FS)/MSR_FS_BASE smoke test (see
# musl/test/tls.c). Entered the normal poc-os way (-e main); it doesn't
# need argv/envp, just a syscall and a %fs-relative memory access.
$(BUILD)/_tls: $(OBJDIR)/musl-test/tls.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/tls.dis
	$(OBJCOPY) --strip-debug $@

# mmap: SYS_mmap/SYS_munmap smoke test (see musl/test/mmap.c).
$(BUILD)/_mmaptest: $(OBJDIR)/musl-test/mmap.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/mmaptest.dis
	$(OBJCOPY) --strip-debug $@

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

# The minimal real-musl object set proven to link and run a genuine
# crt1-started, argv/envp/auxv-fed, TLS-initialized program (see
# musl/test/real_hello.c): startup (crt1, __libc_start_main, TLS init,
# environ/libc globals), the generic syscall_cp/errno/memcpy plumbing
# every musl syscall wrapper sits on top of, and write()/exit()/_Exit()
# themselves. Nowhere near all of musl - no malloc, no stdio/printf, no
# dynamic linking - just what this specific set of source files pulls
# in transitively; grown file-by-file, fixing each undefined-reference
# as it came up, not designed upfront.
MUSL_LIBC_OBJS = \
	$(OBJDIR)/musl/crt/crt1.o \
	$(OBJDIR)/musl/src/env/__libc_start_main.o \
	$(OBJDIR)/musl/src/env/__init_tls.o \
	$(OBJDIR)/musl/src/env/__environ.o \
	$(OBJDIR)/musl/src/internal/libc.o \
	$(OBJDIR)/musl/src/internal/defsysinfo.o \
	$(OBJDIR)/musl/src/internal/syscall_ret.o \
	$(OBJDIR)/musl/src/thread/__syscall_cp.o \
	$(OBJDIR)/musl/src/thread/default_attr.o \
	$(OBJDIR)/musl/src/thread/x86_64/__set_thread_area.o \
	$(OBJDIR)/musl/src/string/memcpy.o \
	$(OBJDIR)/musl/src/errno/__errno_location.o \
	$(OBJDIR)/musl/src/unistd/write.o \
	$(OBJDIR)/musl/src/exit/exit.o \
	$(OBJDIR)/musl/src/exit/_Exit.o \

# real_hello: entry is musl's own _start (crt1.o, via crt_arch.h), not
# poc-os's usual -e main - it has to be launched with SYS_execve (see
# musl/test/runmusl.c), not poc-os's native SYS_exec, since only
# execve() builds the argc/argv/envp/auxv stack crt1 reads.
$(BUILD)/_real_hello: $(OBJDIR)/musl/test/real_hello.o $(MUSL_LIBC_OBJS) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e _start -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/real_hello.dis
	$(OBJCOPY) --strip-debug $@

# runmusl: generic launcher, "runmusl <path> [args...]" - see
# musl/test/runmusl.c. Entered the normal poc-os way (-e main, native
# SYS_exec) since it's poc-os code, not musl code; what it launches
# (SYS_execve on argv[1]) is what needs the musl-style stack.
$(BUILD)/_runmusl: $(OBJDIR)/musl-test/runmusl.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/runmusl.dis
	$(OBJCOPY) --strip-debug $@

# Adds musl's "oldmalloc" backend (musl/src/malloc/oldmalloc/malloc.c -
# chosen over the default mallocng backend for being one file, simpler,
# and forgiving of poc-os's minimal mmap/munmap) on top of
# MUSL_LIBC_OBJS: real malloc/calloc/realloc/free, real SYS_brk (which
# oldmalloc calls directly, not through any sbrk()-style wrapper - see
# kernel/sysproc.c's sys_brk()), and the handful of files that turned
# out to be needed to link it (strlen, __lock/__wait for its
# currently-always-uncontended locking, madvise/mremap - both
# kernel-side stubs, see include/syscall.h).
MUSL_MALLOC_OBJS = \
	$(OBJDIR)/musl/src/malloc/oldmalloc/malloc.o \
	$(OBJDIR)/musl/src/malloc/lite_malloc.o \
	$(OBJDIR)/musl/src/malloc/calloc.o \
	$(OBJDIR)/musl/src/malloc/realloc.o \
	$(OBJDIR)/musl/src/malloc/free.o \
	$(OBJDIR)/musl/src/malloc/replaced.o \
	$(OBJDIR)/musl/src/mman/mmap.o \
	$(OBJDIR)/musl/src/mman/munmap.o \
	$(OBJDIR)/musl/src/mman/madvise.o \
	$(OBJDIR)/musl/src/mman/mremap.o \
	$(OBJDIR)/musl/src/string/memset.o \
	$(OBJDIR)/musl/src/string/strlen.o \
	$(OBJDIR)/musl/src/thread/__wait.o \
	$(OBJDIR)/musl/src/thread/__lock.o \

$(BUILD)/_real_malloc: $(OBJDIR)/musl/test/real_malloc.o $(MUSL_LIBC_OBJS) $(MUSL_MALLOC_OBJS) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e _start -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/real_malloc.dis
	$(OBJCOPY) --strip-debug $@

# Adds real printf/vfprintf/stdio on top of MUSL_LIBC_OBJS +
# MUSL_MALLOC_OBJS (FILE buffering needs both malloc, for the stream
# buffer, and the syscalls below). %a/%e/%f/%g print a fixed
# "<float-unsupported>" placeholder rather than a real value -
# vfprintf.c is forked (see its fmt_fp() and pop_arg()'s DBL/LDBL
# cases) to avoid all x87/SSE codegen, none of which compiles under
# -mgeneral-regs-only in the first place, the same reason nothing else
# in this build touches floating point; real support needs the kernel
# to save/restore FPU state across a context switch first, which it
# doesn't do at all today. Also needed two more real syscalls beyond
# what MUSL_LIBC_OBJS/MUSL_MALLOC_OBJS required: SYS_writev (stdio's
# actual flush path) and SYS_lseek (both genuinely implemented, not
# stubs - kernel/sysfile.c), plus two stubs (SYS_ioctl, always fails -
# stdio's isatty-for-buffering-mode check; poc-os has no ioctl of any
# kind yet).
MUSL_STDIO_OBJS = \
	$(OBJDIR)/musl/src/stdio/vfprintf.o \
	$(OBJDIR)/musl/src/stdio/printf.o \
	$(OBJDIR)/musl/src/stdio/vprintf.o \
	$(OBJDIR)/musl/src/stdio/__towrite.o \
	$(OBJDIR)/musl/src/stdio/__overflow.o \
	$(OBJDIR)/musl/src/stdio/__stdio_write.o \
	$(OBJDIR)/musl/src/stdio/stdout.o \
	$(OBJDIR)/musl/src/stdio/__lockfile.o \
	$(OBJDIR)/musl/src/stdio/__stdio_exit.o \
	$(OBJDIR)/musl/src/stdio/ofl.o \
	$(OBJDIR)/musl/src/stdio/ofl_add.o \
	$(OBJDIR)/musl/src/stdio/fwrite.o \
	$(OBJDIR)/musl/src/stdio/__stdio_close.o \
	$(OBJDIR)/musl/src/stdio/__stdout_write.o \
	$(OBJDIR)/musl/src/stdio/__stdio_seek.o \
	$(OBJDIR)/musl/src/errno/strerror.o \
	$(OBJDIR)/musl/src/string/strnlen.o \
	$(OBJDIR)/musl/src/string/memchr.o \
	$(OBJDIR)/musl/src/multibyte/wctomb.o \
	$(OBJDIR)/musl/src/multibyte/wcrtomb.o \
	$(OBJDIR)/musl/src/locale/__lctrans.o \
	$(OBJDIR)/musl/src/unistd/lseek.o \

$(BUILD)/_real_printf: $(OBJDIR)/musl/test/real_printf.o $(MUSL_LIBC_OBJS) $(MUSL_MALLOC_OBJS) $(MUSL_STDIO_OBJS) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e _start -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/real_printf.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/mkfs: mkfs/mkfs.c include/fs.h | $(BUILD)
	# -iquote (not -I) so quoted poc headers resolve to include/ while
	# <fcntl.h> etc still resolve to the host's system headers.
	gcc -Werror -Wall -iquote include -o $(BUILD)/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: $(OBJDIR)/user/%.o $(OBJDIR)/kernel/%.o $(OBJDIR)/boot/%.o $(OBJDIR)/musl/%.o $(OBJDIR)/musl-test/%.o

UPROGS=\
	$(BUILD)/_cat\
	$(BUILD)/_echo\
	$(BUILD)/_forktest\
	$(BUILD)/_grep\
	$(BUILD)/_init\
	$(BUILD)/_kill\
	$(BUILD)/_ln\
	$(BUILD)/_ls\
	$(BUILD)/_mkdir\
	$(BUILD)/_rm\
	$(BUILD)/_sh\
	$(BUILD)/_stressfs\
	$(BUILD)/_usertests\
	$(BUILD)/_wc\
	$(BUILD)/_zombie\

# musl-test/ smoke-test binaries (see musl/README and musl/test/*.c):
# x86_64-only, like the rest of the musl port - some of them (notably
# execve_verify.c's _start_asm) use raw 64-bit-register inline asm that
# doesn't compile at all under ARCH=32's -m32, so unlike every other
# UPROGS entry above these are conditional on ARCH rather than always
# built.
ifeq ($(ARCH),64)
UPROGS+=\
	$(BUILD)/_execve_launch\
	$(BUILD)/_execve_verify\
	$(BUILD)/_mmaptest\
	$(BUILD)/_muslhello\
	$(BUILD)/_real_hello\
	$(BUILD)/_real_malloc\
	$(BUILD)/_real_printf\
	$(BUILD)/_runmusl\
	$(BUILD)/_tls\

endif

$(BUILD)/fs.img: $(BUILD)/mkfs $(UPROGS)
	./$(BUILD)/mkfs $(BUILD)/fs.img $(UPROGS)

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
	mkfs/mkfs.c user/ulib.c include/user.h user/cat.c user/echo.c user/forktest.c user/grep.c user/kill.c\
	user/ln.c user/ls.c user/mkdir.c user/rm.c user/stressfs.c user/usertests.c user/wc.c user/zombie.c\
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
