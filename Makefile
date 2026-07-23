# Top-level build: produces the bootable hello-os.iso by combining the
# compiled kernel with the Limine bootloader.

# `override` prevents a caller from renaming the image via the command line.
override IMAGE_NAME := hello-os

# 256MB of guest RAM, and route the serial port to this terminal so
# serial_print() output (see kernel/src/serial.c) is visible when running.
QEMUFLAGS := -m 256M -serial stdio

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

# Boot the built ISO in QEMU as if from a CD-ROM.
.PHONY: run
run: $(IMAGE_NAME).iso
	qemu-system-x86_64 -cdrom $(IMAGE_NAME).iso -boot d $(QEMUFLAGS)

# Remove build outputs but keep the fetched limine/ toolchain (avoids
# re-cloning it on every rebuild).
.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso initrd.tar

# Full clean, including the fetched limine/ directory.
.PHONY: distclean
distclean: clean
	rm -rf limine
