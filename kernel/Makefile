# Builds kernel/src/*.c into the freestanding ELF binary bin/kernel.
# Invoked by the top-level Makefile via `make -C kernel`.

# `override` so a caller can't accidentally clobber the output name
# by passing OUTPUT=... on the command line.
override OUTPUT := kernel

# clang/lld are used (rather than gcc/ld) mainly because clang has
# first-class `-target x86_64-unknown-none-elf` cross-compilation
# support without needing a separately built cross-toolchain.
CC := clang
LD := ld.lld

CFLAGS := -Wall -Wextra -std=gnu11 -O2 -g \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-pic -fno-pie \
    -m64 -march=x86-64 \
    -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -mno-red-zone -mcmodel=kernel \
    -target x86_64-unknown-none-elf
# -ffreestanding:        no hosted stdlib/runtime assumptions (no libc, no main())
# -fno-stack-protector/-check: no stack canaries (they'd need runtime support we don't have)
# -fno-pic/-fno-pie:     absolute addressing; we run at a fixed linked address, not relocatable
# -mno-80387/mmx/sse/sse2: don't let the compiler emit FPU/SIMD instructions,
#                        since we never set up the FPU/SSE state at boot
# -mno-red-zone:         the red zone isn't safe once interrupts can fire on this stack
# -mcmodel=kernel:       assume code+data live in the top 2GB of address space
#                        (matches the link address in linker.ld)
# -target ...-none-elf:  cross-compile for bare-metal x86-64, no OS ABI

# Only search src/ for #included project headers (limine.h, serial.h, font8x8.h).
CPPFLAGS := -I src

LDFLAGS := -m elf_x86_64 -nostdlib -static \
    -z max-page-size=0x1000 --gc-sections \
    -T linker.ld
# -nostdlib:        don't link against libc/crt startup files
# -static:          no dynamic linking (there's no dynamic linker in a kernel)
# -z max-page-size: match linker.ld's page alignment assumption
# --gc-sections:    drop unused sections (keeps the image lean)
# -T linker.ld:     use our layout instead of the default linker script

# Compile every .c file under src/ into a matching .o file under obj/.
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=obj/%.o)

.PHONY: all clean
all: bin/$(OUTPUT)

# Link all object files into the final kernel ELF. Depends on linker.ld
# too, so editing the memory layout triggers a relink.
bin/$(OUTPUT): $(OBJS) linker.ld
	mkdir -p bin
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Pattern rule: compile any src/X.c into obj/X.o.
obj/%.o: src/%.c
	mkdir -p obj
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf bin obj
