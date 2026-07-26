/*
 * Minimal linker script for standalone static ET_EXEC ELF64 userland
 * binaries -- distinct from kernel/linker.ld (which links the kernel
 * itself at the higher-half 0xffffffff80000000). 0x400000 is also
 * ELF_PIE_BASE (process.c) -- where a PIE main executable's segments
 * land once elf_load() adds its load_base -- and is comfortably below
 * VMM_USER_ANON_BASE/VMM_USER_STACK_TOP (kernel/src/vmm.h) so a loaded
 * ELF's segments can never collide with anon memory or the stack region
 * elf.c maps separately.
 */
ENTRY(_start)
SECTIONS
{
    /* Starting the location counter at BASE + SIZEOF_HEADERS (rather
     * than bare BASE) is the standard idiom for making the *first*
     * PT_LOAD segment's file range cover the ELF header and program
     * header table too, not just .text: ld.lld backs the segment's
     * p_vaddr/p_offset up to account for exactly that many bytes,
     * whereas a bare ". = 0x400000;" left a 4KiB gap between the file's
     * start and .text's, entirely outside any PT_LOAD segment -- so
     * elf.c's map_segment() never copied the headers into mapped memory
     * at all, regardless of where __ehdr_start (below) pointed. mlibc's
     * static-binary startup path (options/rtld/generic/main.cpp's
     * interpreterMain(), under MLIBC_STATIC_BUILD) reads its own program
     * headers by dereferencing __ehdr_start directly, so this matters
     * even though nothing here uses dynamic linking.
     */
    . = 0x400000 + SIZEOF_HEADERS;
    __ehdr_start = 0x400000;
    .text : { *(.text*) }
    /* crti.o + crtbegin.o + crtend.o + crtn.o concatenate into one
     * complete _init/_fini function each -- see crti.S's doc comment. */
    .init : { KEEP(*(.init)) }
    .fini : { KEEP(*(.fini)) }
    .rodata : { *(.rodata*) }

    /* mlibc's rtld (options/rtld/generic/linker.cpp) walks
     * __init_array_start..__init_array_end (etc.) directly to run global
     * constructors -- both the sections and these boundary symbols have
     * to be explicit here, since (unlike ld.lld, which synthesizes them
     * even for a custom SECTIONS block) GNU ld only creates them when a
     * script names the sections itself. */
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
    /* Legacy .ctors/.dtors -- distinct from .init_array/.fini_array
     * above. GCC's crtbegin.o (x86_64-elf's generic target config, not
     * configured with a real OS's init_array-aware crtstuff) still emits
     * __do_global_ctors_aux, which walks a `__CTOR_LIST__` array from the
     * *top* down until it reads a -1 sentinel -- bash's build was the
     * first to actually hit this (a plain C program has no global
     * constructors to register at all), immediately after which
     * __do_global_ctors_aux fell off the end of a nonexistent .ctors
     * section into unrelated memory and eventually called address -1.
     * The three-way KEEP/EXCLUDE_FILE split (standard GNU ld idiom, see
     * ld's own default SECTIONS script) puts crtbegin.o's own leading
     * entry first, real per-TU ctors in the middle, and crtend.o's
     * trailing -1 terminator last -- letting any input file's .ctors
     * fall through the middle rule while still guaranteeing crtbegin's/
     * crtend's own contributions land at the two ends. */
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

    /* Not actually used (no exception unwinding support), but crtbegin.o
     * (GCC's own, linked in for __dso_handle/.init_array registration --
     * see above) has internal PC-relative references into its own
     * .eh_frame content; discarding the section outright breaks those
     * even though nothing here ever unwinds through it. */
    .eh_frame : { KEEP(*(.eh_frame)) }

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
