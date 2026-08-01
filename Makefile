# The first rule in the file is normally make's default goal, but that's
# the build/-directory-creation rule below (needed as an order-only
# prerequisite elsewhere) rather than a real build target - pin the
# default explicitly so a bare `make` builds the OS image like `make all`.
.DEFAULT_GOAL := all

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
	$(OBJDIR)/kernel/swtch.o\
	$(OBJDIR)/kernel/syscall.o\
	$(OBJDIR)/kernel/sysfile.o\
	$(OBJDIR)/kernel/sysproc.o\
	$(OBJDIR)/kernel/trapasm.o\
	$(OBJDIR)/kernel/trap.o\
	$(OBJDIR)/kernel/uart.o\
	$(OBJDIR)/kernel/vectors.o\
	$(OBJDIR)/kernel/vm.o\
	$(OBJDIR)/kernel/x86.o\

# Cross-compiling (e.g., on Mac OS X)
# TOOLPREFIX = i386-jos-elf

# Using native tools (e.g., on X86 Linux)
#TOOLPREFIX =

# Try to infer the correct TOOLPREFIX if not set. i686-elf-* is the
# prefix used by the Homebrew i686-elf-gcc/i686-elf-binutils packages
# (the common way to get an i386 ELF cross toolchain on modern macOS).
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if i686-elf-objdump -i 2>&1 | grep 'elf32-i386' >/dev/null 2>&1; \
	then echo 'i686-elf-'; \
	elif objdump -i 2>&1 | grep 'elf32-i386' >/dev/null 2>&1; \
	then echo ''; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find an i386-*-elf version of GCC/binutils." 1>&2; \
	echo "*** Is the directory with i686-elf-gcc in your PATH?" 1>&2; \
	echo "*** If your i386-*-elf toolchain is installed with a command" 1>&2; \
	echo "*** prefix other than 'i686-elf-', set your TOOLPREFIX" 1>&2; \
	echo "*** environment variable to that prefix and run 'make' again." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

# If the makefile can't find QEMU, specify its path here
# QEMU = qemu-system-i386

# Try to infer the correct QEMU
ifndef QEMU
QEMU = $(shell if which qemu > /dev/null; \
	then echo qemu; exit; \
	elif which qemu-system-i386 > /dev/null; \
	then echo qemu-system-i386; exit; \
	elif which qemu-system-x86_64 > /dev/null; \
	then echo qemu-system-x86_64; exit; \
	else \
	qemu=/Applications/Q.app/Contents/MacOS/i386-softmmu.app/Contents/MacOS/i386-softmmu; \
	if test -x $$qemu; then echo $$qemu; exit; fi; fi; \
	echo "***" 1>&2; \
	echo "*** Error: Couldn't find a working QEMU executable." 1>&2; \
	echo "*** Is the directory containing the qemu binary in your PATH" 1>&2; \
	echo "*** or have you tried setting the QEMU variable in Makefile?" 1>&2; \
	echo "***" 1>&2; exit 1)
endif

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# NASM assembles all the .asm (Intel-syntax) sources. The shared C headers
# in include/ (constants, struct layouts) are still expanded into them with
# the C preprocessor before NASM ever sees the file - see the %.o: %.asm
# rules below.
NASM = nasm
NASMFLAGS = -f elf32 -g

# All headers live in include/, shared by boot/, kernel/, user/ and mkfs/.
CPPFLAGS = -Iinclude

CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -O2 -Wall -MD -ggdb -m32 -Werror -fno-omit-frame-pointer -Wno-error=array-bounds -Wno-error=infinite-recursion -Wno-error=unused-but-set-variable
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
ASFLAGS = -m32 -gdwarf-2 -Wa,-divide
# FreeBSD ld wants ``elf_i386_fbsd''
LDFLAGS += -m $(shell $(LD) -V | grep elf_i386 2>/dev/null | head -n 1)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

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
OBJDIR = $(BUILD)/obj

$(BUILD) $(OBJDIR)/boot $(OBJDIR)/kernel $(OBJDIR)/user:
	mkdir -p $@

# Compile a C source into build/obj/<dir>/<name>.o, keeping the same
# boot/kernel/user split the sources themselves use.
$(OBJDIR)/boot/%.o: boot/%.c | $(OBJDIR)/boot
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR)/kernel/%.o: kernel/%.c | $(OBJDIR)/kernel
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR)/user/%.o: user/%.c | $(OBJDIR)/user
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Assemble a NASM source the same way: first expand #include/#define from
# include/ with the C preprocessor (cpp doesn't care about NASM vs GAS
# mnemonics, it just does text substitution), then hand the result to nasm.
$(OBJDIR)/boot/%.o: boot/%.asm | $(OBJDIR)/boot
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(NASMFLAGS) -o $@ $(@:.o=.i)

$(OBJDIR)/kernel/%.o: kernel/%.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(NASMFLAGS) -o $@ $(@:.o=.i)

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
# the 2-byte 0x55AA signature), and -O2 (the default in $(CFLAGS)) alone
# generates code too large to fit, so this keeps the lighter -O the
# original boot Makefile always used here, along with -nostdinc since
# the boot loader is freestanding.
$(OBJDIR)/boot/bootmain.o: boot/bootmain.c | $(OBJDIR)/boot
	$(CC) $(CFLAGS) $(CPPFLAGS) -fno-pic -O -nostdinc -c -o $@ $<

$(BUILD)/bootblock: $(OBJDIR)/boot/bootasm.o $(OBJDIR)/boot/bootmain.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7C00 -o $(OBJDIR)/boot/bootblock.o $(OBJDIR)/boot/bootasm.o $(OBJDIR)/boot/bootmain.o
	$(OBJDUMP) -S $(OBJDIR)/boot/bootblock.o > $(BUILD)/bootblock.dis
	$(OBJCOPY) -S -O binary -j .text $(OBJDIR)/boot/bootblock.o $(BUILD)/bootblock
	./boot/sign.pl $(BUILD)/bootblock

# entryother and initcode are raw binary blobs the kernel embeds with
# -b binary (see kernel/main.c's and kernel/proc.c's matching
# _binary_build_..._start symbols) rather than programs run standalone,
# so unlike everything else in $(OBJS)/ULIB they need their own two-step
# ld+objcopy recipe instead of just landing in a link line. They're
# final build products, not intermediates, so - like bootblock, kernel,
# and mkfs - they live directly in build/, not build/obj/.
$(BUILD)/entryother: $(OBJDIR)/kernel/entryother.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o $(OBJDIR)/kernel/bootblockother.o $(OBJDIR)/kernel/entryother.o
	$(OBJCOPY) -S -O binary -j .text $(OBJDIR)/kernel/bootblockother.o $(BUILD)/entryother
	$(OBJDUMP) -S $(OBJDIR)/kernel/bootblockother.o > $(BUILD)/entryother.dis

$(BUILD)/initcode: $(OBJDIR)/user/initcode.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $(OBJDIR)/user/initcode.out $(OBJDIR)/user/initcode.o
	$(OBJCOPY) -S -O binary $(OBJDIR)/user/initcode.out $(BUILD)/initcode
	$(OBJDUMP) -S $(OBJDIR)/user/initcode.o > $(BUILD)/initcode.dis

$(BUILD)/kernel: $(OBJS) $(OBJDIR)/kernel/entry.o $(BUILD)/entryother $(BUILD)/initcode kernel/kernel.ld | $(BUILD)
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $(BUILD)/kernel $(OBJDIR)/kernel/entry.o $(OBJS) -b binary $(BUILD)/initcode $(BUILD)/entryother
	$(OBJDUMP) -S $(BUILD)/kernel > $(BUILD)/kernel.dis
	$(OBJDUMP) -t $(BUILD)/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/kernel.sym

# kernelmemfs is a copy of kernel that maintains the
# disk image in memory instead of writing to a disk.
# This is not so useful for testing persistent storage or
# exploring disk buffering implementations, but it is
# great for testing the kernel on real hardware without
# needing a scratch disk.
MEMFSOBJS = $(filter-out $(OBJDIR)/kernel/ide.o,$(OBJS)) $(OBJDIR)/kernel/memide.o
$(BUILD)/kernelmemfs: $(MEMFSOBJS) $(OBJDIR)/kernel/entry.o $(BUILD)/entryother $(BUILD)/initcode kernel/kernel.ld $(BUILD)/fs.img | $(BUILD)
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $(BUILD)/kernelmemfs $(OBJDIR)/kernel/entry.o $(MEMFSOBJS) -b binary $(BUILD)/initcode $(BUILD)/entryother $(BUILD)/fs.img
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
	./kernel/vectors.pl > $(OBJDIR)/kernel/vectors.asm

$(OBJDIR)/kernel/vectors.o: $(OBJDIR)/kernel/vectors.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(OBJDIR)/kernel/vectors.i $(OBJDIR)/kernel/vectors.asm
	$(NASM) $(NASMFLAGS) -o $@ $(OBJDIR)/kernel/vectors.i

ULIB = $(OBJDIR)/user/ulib.o $(OBJDIR)/user/usys.o $(OBJDIR)/user/printf.o $(OBJDIR)/user/umalloc.o $(OBJDIR)/kernel/x86.o

$(BUILD)/_%: $(OBJDIR)/user/%.o $(ULIB) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/$*.dis
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/$*.sym

$(BUILD)/_forktest: $(OBJDIR)/user/forktest.o $(ULIB) | $(BUILD)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $(BUILD)/_forktest $(OBJDIR)/user/forktest.o $(OBJDIR)/user/ulib.o $(OBJDIR)/user/usys.o $(OBJDIR)/kernel/x86.o
	$(OBJDUMP) -S $(BUILD)/_forktest > $(BUILD)/forktest.dis

$(BUILD)/mkfs: mkfs/mkfs.c include/fs.h | $(BUILD)
	# -iquote (not -I) so quoted poc headers resolve to include/ while
	# <fcntl.h> etc still resolve to the host's system headers.
	gcc -Werror -Wall -iquote include -o $(BUILD)/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: $(OBJDIR)/user/%.o $(OBJDIR)/kernel/%.o $(OBJDIR)/boot/%.o

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
