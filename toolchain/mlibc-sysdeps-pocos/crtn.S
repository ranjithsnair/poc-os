/*
 * The *epilogue* half of `_init`/`_fini` -- see crti.S's doc comment for
 * the full explanation. Must be the very last thing linked so its
 * `leave; ret` closes out whatever crti.S/crtbegin.o/crtend.o
 * contributed to the same two sections earlier in the link.
 */
.section .init
    leave
    ret

.section .fini
    leave
    ret

.section .note.GNU-stack,"",@progbits
