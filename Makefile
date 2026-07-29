BUILD_DIR := build
BOOT_BIN  := $(BUILD_DIR)/boot.bin

.PHONY: all run clean

all: $(BOOT_BIN)

$(BOOT_BIN): boot/boot.asm | $(BUILD_DIR)
	nasm -f bin $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(BOOT_BIN)
	qemu-system-x86_64 -drive format=raw,file=$(BOOT_BIN)

clean:
	rm -rf $(BUILD_DIR)
