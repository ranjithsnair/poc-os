# Top-level build: produces the bootable hello-os.iso by combining the
# compiled kernel with the Limine bootloader.

# `override` prevents a caller from renaming the image via the command line.
override IMAGE_NAME := hello-os

# Cross toolchain versions/paths (see the cross-binutils/cross-gcc rules
# below) -- defined up top since the userland/%.gcc.elf rule references
# GCC_CRTDIR before make would otherwise have seen GCC_VERSION assigned.
BINUTILS_VERSION := 2.44
GCC_VERSION := 14.2.0
TOOLCHAIN_PREFIX := $(abspath toolchain/cross)
TOOLCHAIN_SYSROOT := $(abspath toolchain/sysroot)

# 256MB of guest RAM, route the serial port to this terminal so
# serial_print() output (see kernel/src/serial.c) is visible when running,
# and attach disk.img as a virtio-blk drive (kernel/src/virtio_blk.c /
# fat32.c) -- the writable filesystem process-visible files live on.
# -display none: PoC-OS is a plain serial/terminal console (no framebuffer
# console, no mouse) -- there's nothing to show in a graphical window, so
# skip opening one at all.
QEMUFLAGS := -m 256M -serial stdio -display none \
	-drive file=disk.img,if=none,format=raw,id=disk0 \
	-device virtio-blk-pci,drive=disk0

.PHONY: all
all: $(IMAGE_NAME).iso

# Delegates to kernel/Makefile to build kernel/bin/kernel.
.PHONY: kernel
kernel:
	$(MAKE) -C kernel

# Builds the initrd: a USTAR archive bundling every file under initrd/,
# which kernel/src/tarfs.c reads at boot (see the module_path directive
# in limine.conf). --format=ustar pins the exact on-disk layout tarfs.c's
# parser was written and tested against — GNU tar's default format
# differs in some header fields, so this flag isn't optional.
INITRD_FILES := $(wildcard initrd/*)
initrd.tar: $(INITRD_FILES)
	tar --format=ustar -cf $@ -C initrd $(notdir $(INITRD_FILES))

# Builds standalone userland test ELFs (real, independently linked
# executables -- unlike kernel/src/*_test.S's in-kernel-image flat blobs)
# used to exercise SYS_EXECVE. See userland/README-ish comment in
# exec_target.S for why this can't just be another flat blob.
userland/%.elf: userland/%.S userland/linker.ld
	clang -target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector \
		-fno-stack-check -fno-pic -fno-pie -m64 -march=x86-64 \
		-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
		-c $< -o $(<:.S=.o)
	ld.lld -m elf_x86_64 -nostdlib -static -T userland/linker.ld $(<:.S=.o) -o $@

# Same, but for a C program linked against our mlibc port (see
# "mlibc-sysroot" below) instead of being a hand-written flat/raw-ELF
# blob -- this is Phase 1's actual deliverable: proof a normal C program
# using mlibc's printf() runs on PoC-OS.
MLIBC_SYSROOT := toolchain/sysroot
userland/%.elf: userland/%.c userland/linker.ld mlibc-sysroot
	clang -target x86_64-unknown-none-elf -ffreestanding -fno-pic -fno-pie \
		-m64 -march=x86-64 -D_GNU_SOURCE \
		-isystem $(MLIBC_SYSROOT)/usr/include \
		-c $< -o $(<:.c=.o)
	ld.lld -m elf_x86_64 -nostdlib -static --gc-sections -T userland/linker.ld \
		$(MLIBC_SYSROOT)/usr/lib/crt1.o $(<:.c=.o) $(MLIBC_SYSROOT)/usr/lib/libc.a \
		-o $@

# Builds the writable FAT32 disk image (kernel/src/fat32.c mounts this at
# boot via kernel/src/virtio_blk.c) from disk_root/, a staging directory
# populated with whatever userland binaries/files need to be on it.
# 300MiB is comfortably over FAT32's 65525-cluster minimum at the image
# builder's 4KiB clusters (see tools/mkfat32.py) -- it's a sparse-ish
# build (mostly zeros past the FAT/data actually used), not 300MiB
# actually written to the host disk's own filesystem.
disk.img: userland/exec_target.elf userland/hello_libc.elf userland/hello_libc.gcc.elf \
		tools/mkfat32.py
	rm -rf disk_root
	mkdir -p disk_root
	cp userland/exec_target.elf disk_root/exectgt
	cp userland/hello_libc.elf disk_root/hellolib
	cp userland/hello_libc.gcc.elf disk_root/hellogcc
	python3 tools/mkfat32.py $@ 300 disk_root
	rm -rf disk_root

# --- Phase 2 verification: the same Phase-1 hello_libc.c program, but
# compiled+linked with the real cross GCC (below) instead of clang,
# confirming --with-sysroot/libgcc specs wiring is correct end to end.
# GCC's own --with-sysroot bakes in $(MLIBC_SYSROOT) already (see
# `cross-gcc` target's configure line), so -lc/-lgcc resolve on their own
# -- no -isystem/-L needed the way the clang rule above requires.
CROSS_BIN := toolchain/cross/bin
CROSS_GCC := $(CROSS_BIN)/x86_64-elf-gcc
GCC_CRTDIR := toolchain/cross/lib/gcc/x86_64-elf/$(GCC_VERSION)
userland/%.gcc.elf: userland/%.c userland/linker.ld mlibc-sysroot cross-gcc
	$(CROSS_GCC) -ffreestanding -fno-pic -fno-pie -m64 \
		-c $< -o $(<:.c=.gcc.o)
	$(CROSS_GCC) -T userland/linker.ld -nostdlib -static \
		$(MLIBC_SYSROOT)/usr/lib/crt1.o $(MLIBC_SYSROOT)/usr/lib/crti.o \
		$(GCC_CRTDIR)/crtbegin.o $(<:.c=.gcc.o) \
		-lc $(GCC_CRTDIR)/crtend.o $(MLIBC_SYSROOT)/usr/lib/crtn.o \
		-o $@

# --- mlibc (Phase 1 of the plan: bring up a real userspace on PoC-OS) ---
#
# Fetches mlibc (not committed, like limine/), overlays our own sysdeps
# port (toolchain/mlibc-sysdeps-pocos/ -- persistent, version-controlled)
# into mlibc/sysdeps/pocos, patches mlibc's own meson.build to recognize
# it (see tools/setup_mlibc.py), configures it with Meson against
# toolchain/pocos.cross-file, builds it with Ninja, and installs headers/
# libc.a/crt1.o into a local sysroot (toolchain/sysroot/) userland
# programs compile and link against above.
#
# llvm-ar/llvm-ranlib (not macOS's native ones, which don't understand
# ELF object files at all -- see toolchain/pocos.cross-file's doc
# comment) come from `brew install llvm`.
LLVM_BIN := $(shell brew --prefix llvm 2>/dev/null)/bin

mlibc/meson.build:
	git clone --depth 1 https://github.com/managarm/mlibc.git mlibc

# Re-runs (wiping and reconfiguring build-pocos/) whenever our sysdeps
# port, the cross-file, or the setup/generator scripts change; a no-op
# rebuild otherwise, so plain `make mlibc-sysroot` after an unrelated
# change is fast (ninja/meson install below are already incremental).
# -type f only (not -o -type l): the abi-bits/*.h entries are symlinks
# that are *intentionally* dangling from this location (they resolve
# relative to mlibc/sysdeps/pocos/, only valid once copied there by
# setup_mlibc.py below) -- letting Make see them as prerequisites makes
# it stat() each one, find it broken, and refuse to proceed at all.
MLIBC_SYSDEPS_SRC := $(shell find toolchain/mlibc-sysdeps-pocos -type f)
mlibc/.pocos-setup: mlibc/meson.build tools/setup_mlibc.py tools/gen_mlibc_stubs.py \
		$(MLIBC_SYSDEPS_SRC) toolchain/pocos.cross-file
	python3 tools/setup_mlibc.py mlibc toolchain/mlibc-sysdeps-pocos
	rm -rf mlibc/build-pocos
	cd mlibc && PATH="$(LLVM_BIN):$$PATH" meson setup build-pocos \
		--cross-file ../toolchain/pocos.cross-file -Dprefix=/usr \
		-Dlinux_option=disabled -Dglibc_option=disabled -Dbsd_option=disabled \
		-Dlibgcc_dependency=false -Ddefault_library=static
	touch $@

.PHONY: mlibc-sysroot
mlibc-sysroot: mlibc/.pocos-setup
	cd mlibc/build-pocos && PATH="$(LLVM_BIN):$$PATH" ninja
	cd mlibc/build-pocos && PATH="$(LLVM_BIN):$$PATH" meson install --destdir ../../$(MLIBC_SYSROOT)

# --- Phase 2 of the plan: a real cross binutils + GCC targeting
# x86_64-elf, built to run on this macOS host. Two-stage-bootstrap-free
# here because mlibc (Phase 1) is built with clang against the cross-file
# above, not with this GCC -- so unlike a from-scratch OSDev toolchain,
# GCC only ever needs to compile against an *already-populated* sysroot,
# never bootstrap it. Not committed (toolchain/cross, build-binutils,
# build-gcc, src are all in .gitignore like mlibc/limine), so this is
# also what makes the whole toolchain reproducible from a clean checkout.
#
# --with-system-zlib on both: the bundled-zlib fallback's zutil.c
# declares zError(err) old-style-K&R, which collides with the macOS SDK's
# own _stdio.h and fails to compile otherwise.
toolchain/src/binutils-$(BINUTILS_VERSION).tar.xz:
	mkdir -p toolchain/src
	curl -L -o $@ https://ftp.gnu.org/gnu/binutils/binutils-$(BINUTILS_VERSION).tar.xz

# Order-only (the `|`) on purpose: a downloaded tarball's own mtime is
# whenever curl happened to run, which is almost always *later* than the
# release-day mtimes preserved on the files tar extracts from it -- a
# normal prerequisite here would make Make think the extracted configure
# is perpetually out-of-date relative to the tarball and re-extract (and
# so re-trigger the full binutils/gcc build below) on every invocation.
toolchain/src/binutils-$(BINUTILS_VERSION)/configure: | toolchain/src/binutils-$(BINUTILS_VERSION).tar.xz
	tar -C toolchain/src -xf toolchain/src/binutils-$(BINUTILS_VERSION).tar.xz
	touch $@

toolchain/src/gcc-$(GCC_VERSION).tar.xz:
	mkdir -p toolchain/src
	curl -L -o $@ https://ftp.gnu.org/gnu/gcc/gcc-$(GCC_VERSION)/gcc-$(GCC_VERSION).tar.xz

toolchain/src/gcc-$(GCC_VERSION)/configure: | toolchain/src/gcc-$(GCC_VERSION).tar.xz
	tar -C toolchain/src -xf toolchain/src/gcc-$(GCC_VERSION).tar.xz
	touch $@

# llvm-ar/llvm-ranlib, not macOS's native ar/ranlib, which can't index
# ELF archives at all -- binutils' own libiberty/bfd build needs a
# working ranlib on the *host* side too.
toolchain/cross/bin/x86_64-elf-ld: toolchain/src/binutils-$(BINUTILS_VERSION)/configure
	rm -rf toolchain/build-binutils
	mkdir -p toolchain/build-binutils
	cd toolchain/build-binutils && PATH="$(LLVM_BIN):$$PATH" \
		../src/binutils-$(BINUTILS_VERSION)/configure \
		--target=x86_64-elf --prefix=$(TOOLCHAIN_PREFIX) \
		--with-sysroot=$(TOOLCHAIN_SYSROOT) \
		--disable-nls --disable-werror --with-system-zlib
	$(MAKE) -C toolchain/build-binutils -j$$(sysctl -n hw.ncpu)
	$(MAKE) -C toolchain/build-binutils install

# --without-headers: this GCC only ever needs to build its own
# freestanding libgcc, never libc itself (that's mlibc, built separately
# by clang) -- but it still needs $(TOOLCHAIN_SYSROOT) to exist (even
# empty) since --with-sysroot is baked into the configure line so the
# *installed*, final x86_64-elf-gcc resolves -lc/-isystem against
# toolchain/sysroot/usr automatically once mlibc-sysroot populates it.
.PHONY: cross-gcc
cross-gcc: toolchain/cross/bin/x86_64-elf-gcc
toolchain/cross/bin/x86_64-elf-gcc: toolchain/cross/bin/x86_64-elf-ld toolchain/src/gcc-$(GCC_VERSION)/configure
	mkdir -p $(TOOLCHAIN_SYSROOT)
	rm -rf toolchain/build-gcc
	mkdir -p toolchain/build-gcc
	cd toolchain/build-gcc && PATH="$(TOOLCHAIN_PREFIX)/bin:$(LLVM_BIN):$$PATH" \
		../src/gcc-$(GCC_VERSION)/configure \
		--target=x86_64-elf --prefix=$(TOOLCHAIN_PREFIX) \
		--with-sysroot=$(TOOLCHAIN_SYSROOT) \
		--with-gmp=/usr/local/opt/gmp --with-mpfr=/usr/local/opt/mpfr \
		--with-mpc=/usr/local/opt/libmpc --with-system-zlib \
		--disable-nls --enable-languages=c --without-headers \
		--disable-shared --disable-threads --disable-libssp \
		--disable-libquadmath --disable-libgomp --disable-libatomic \
		--disable-libitm --disable-libvtv --disable-libstdcxx \
		--disable-decimal-float --disable-bootstrap
	$(MAKE) -C toolchain/build-gcc -j$$(sysctl -n hw.ncpu) all-gcc all-target-libgcc
	$(MAKE) -C toolchain/build-gcc install-gcc install-target-libgcc

# Fetches and builds the Limine bootloader tooling (the `limine` binary
# used below to install the BIOS boot record) if it isn't present yet.
# Pinned to the v9.x binary release branch so the vendored copy matches
# the boot protocol version kernel/src/limine.h implements.
limine/limine:
	rm -rf limine
	git clone https://github.com/limine-bootloader/limine.git --branch=v9.x-binary --depth=1 limine
	$(MAKE) -C limine

# Assemble a hybrid BIOS+UEFI bootable ISO:
#   1. build the kernel and fetch/build limine (prerequisites)
#   2. lay out an ISO root with the kernel, Limine's config, and its
#      BIOS/UEFI boot files in the paths limine.conf expects
#   3. use xorriso to package iso_root into an El Torito bootable ISO,
#      with a BIOS boot catalog entry and a UEFI boot partition
#   4. stamp a BIOS boot record onto the finished ISO via `limine bios-install`
#      so it's also bootable on legacy BIOS, not just UEFI
$(IMAGE_NAME).iso: kernel limine/limine initrd.tar
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	cp kernel/bin/kernel iso_root/boot/
	cp initrd.tar iso_root/boot/
	cp limine.conf iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

# Boot the built ISO in QEMU as if from a CD-ROM, with disk.img attached
# as the writable virtio-blk drive.
.PHONY: run
run: $(IMAGE_NAME).iso disk.img
	qemu-system-x86_64 -cdrom $(IMAGE_NAME).iso -boot d $(QEMUFLAGS)

# Remove build outputs but keep the fetched limine/ toolchain (avoids
# re-cloning it on every rebuild).
.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso initrd.tar disk.img disk_root userland/*.o userland/*.elf

# Full clean, including the fetched limine/ and mlibc/ directories and
# the installed sysroot built from the latter.
.PHONY: distclean
distclean: clean
	rm -rf limine mlibc $(MLIBC_SYSROOT)
