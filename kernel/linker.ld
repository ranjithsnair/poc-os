/* Output as a 64-bit x86-64 ELF executable. */
OUTPUT_FORMAT(elf64-x86-64)
/* The bootloader jumps to this symbol after loading the kernel. */
ENTRY(kmain)

/* Program headers: tell the loader (Limine) which permissions each
 * segment needs. Flag bits are the standard ELF PF_* values:
 * 1=PF_X (execute), 2=PF_W (write), 4=PF_R (read). */
PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;             /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ;  /* Write + Read */
}

SECTIONS
{
    /* Kernel loaded at the top of the higher half, per the Limine protocol. */
    . = 0xffffffff80000000;

    /* Executable code: everything the compiler emitted into .text,
     * including compiler-generated sub-sections like .text.unlikely. */
    .text : {
        *(.text .text.*)
    } :text

    /* Page-align before switching to a segment with different
     * permissions, since protections are enforced per page. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Read-only data: string literals, const globals, etc. */
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Writable data, including the Limine request structs. The three
     * marker sections here must stay in this order and adjacent so
     * Limine can scan from the start marker to the end marker and find
     * every request struct main.c placed in ".requests" in between. */
    .data : {
        *(.requests_start_marker)
        *(.requests)
        *(.requests_end_marker)

        *(.data .data.*)
    } :data

    /* Zero-initialized data: not stored in the ELF file, just reserves
     * space that the loader zeroes. Placed in the same segment as .data
     * since both need write permission. */
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /* Discard sections we don't need in a freestanding kernel: .eh_frame
     * (C++ exception unwinding info) and ELF notes. Keeping them would
     * just waste space since nothing in this kernel unwinds exceptions. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}
