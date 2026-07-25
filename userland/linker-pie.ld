/*
 * Linker script for PIE (ET_DYN) userland executables linked against the
 * shared mlibc build (toolchain/sysroot-shared/, see the top-level
 * Makefile's userland/%.dyn.elf rule) -- identical to linker.ld except
 * the location counter starts at bare SIZEOF_HEADERS (link-time-relative
 * addresses starting at 0) instead of a fixed 0x400000, since a PIE's
 * segments get relocated to wherever the kernel's ELF loader (elf.c's
 * `load_base`) decides to place them at load time, unlike a plain
 * ET_EXEC's already-absolute addresses. See linker.ld's own comments for
 * why each section/symbol here is arranged the way it is -- that
 * reasoning is unchanged, only the base address differs.
 */
ENTRY(_start)
SECTIONS
{
    . = SIZEOF_HEADERS;
    __ehdr_start = 0;
    .text : { *(.text*) }
    .init : { KEEP(*(.init)) }
    .fini : { KEEP(*(.fini)) }
    .rodata : { *(.rodata*) }

    .init_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }
    .ctors : {
        KEEP (*crtbegin*.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend*.o) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
    }
    .dtors : {
        KEEP (*crtbegin*.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend*.o) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    }

    .data : { *(.data*) }
    .bss : { *(.bss*) }

    .dynamic : { *(.dynamic) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rela.dyn : { *(.rela.*) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }

    .eh_frame : { KEEP(*(.eh_frame)) }

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
